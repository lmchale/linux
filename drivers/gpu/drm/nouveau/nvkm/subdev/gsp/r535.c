/*
 * Copyright 2023 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include <rm/rpc.h>
#include "priv.h"

#include <core/pci.h>
#include <subdev/timer.h>
#include <subdev/vfn.h>
#include <engine/fifo/chan.h>
#include <engine/sec2.h>
#include <nvif/log.h>

#include <nvfw/fw.h>

#include <nvrm/nvtypes.h>
#include <nvrm/535.113.01/common/sdk/nvidia/inc/class/cl0000.h>
#include <nvrm/535.113.01/common/sdk/nvidia/inc/class/cl0005.h>
#include <nvrm/535.113.01/common/sdk/nvidia/inc/class/cl0080.h>
#include <nvrm/535.113.01/common/sdk/nvidia/inc/class/cl2080.h>
#include <nvrm/535.113.01/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080event.h>
#include <nvrm/535.113.01/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gpu.h>
#include <nvrm/535.113.01/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080internal.h>
#include <nvrm/535.113.01/common/sdk/nvidia/inc/nvos.h>
#include <nvrm/535.113.01/common/shared/msgq/inc/msgq/msgq_priv.h>
#include <nvrm/535.113.01/common/uproc/os/common/include/libos_init_args.h>
#include <nvrm/535.113.01/nvidia/arch/nvalloc/common/inc/gsp/gsp_fw_sr_meta.h>
#include <nvrm/535.113.01/nvidia/arch/nvalloc/common/inc/gsp/gsp_fw_wpr_meta.h>
#include <nvrm/535.113.01/nvidia/arch/nvalloc/common/inc/rmRiscvUcode.h>
#include <nvrm/535.113.01/nvidia/arch/nvalloc/common/inc/rmgspseq.h>
#include <nvrm/535.113.01/nvidia/generated/g_allclasses.h>
#include <nvrm/535.113.01/nvidia/generated/g_os_nvoc.h>
#include <nvrm/535.113.01/nvidia/generated/g_rpc-structures.h>
#include <nvrm/535.113.01/nvidia/inc/kernel/gpu/gsp/gsp_fw_heap.h>
#include <nvrm/535.113.01/nvidia/inc/kernel/gpu/gsp/gsp_init_args.h>
#include <nvrm/535.113.01/nvidia/inc/kernel/gpu/gsp/gsp_static_config.h>
#include <nvrm/535.113.01/nvidia/inc/kernel/gpu/intr/engine_idx.h>
#include <nvrm/535.113.01/nvidia/kernel/inc/vgpu/rpc_global_enums.h>

#include <linux/acpi.h>
#include <linux/ctype.h>
#include <linux/parser.h>

extern struct dentry *nouveau_debugfs_root;

static void
r535_gsp_event_dtor(struct nvkm_gsp_event *event)
{
	struct nvkm_gsp_device *device = event->device;
	struct nvkm_gsp_client *client = device->object.client;
	struct nvkm_gsp *gsp = client->gsp;

	mutex_lock(&gsp->client_id.mutex);
	if (event->func) {
		list_del(&event->head);
		event->func = NULL;
	}
	mutex_unlock(&gsp->client_id.mutex);

	nvkm_gsp_rm_free(&event->object);
	event->device = NULL;
}

static int
r535_gsp_device_event_get(struct nvkm_gsp_event *event)
{
	struct nvkm_gsp_device *device = event->device;
	NV2080_CTRL_EVENT_SET_NOTIFICATION_PARAMS *ctrl;

	ctrl = nvkm_gsp_rm_ctrl_get(&device->subdevice,
				    NV2080_CTRL_CMD_EVENT_SET_NOTIFICATION, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->event = event->id;
	ctrl->action = NV2080_CTRL_EVENT_SET_NOTIFICATION_ACTION_REPEAT;
	return nvkm_gsp_rm_ctrl_wr(&device->subdevice, ctrl);
}

static int
r535_gsp_device_event_ctor(struct nvkm_gsp_device *device, u32 handle, u32 id,
			   nvkm_gsp_event_func func, struct nvkm_gsp_event *event)
{
	struct nvkm_gsp_client *client = device->object.client;
	struct nvkm_gsp *gsp = client->gsp;
	NV0005_ALLOC_PARAMETERS *args;
	int ret;

	args = nvkm_gsp_rm_alloc_get(&device->subdevice, handle,
				     NV01_EVENT_KERNEL_CALLBACK_EX, sizeof(*args),
				     &event->object);
	if (IS_ERR(args))
		return PTR_ERR(args);

	args->hParentClient = client->object.handle;
	args->hSrcResource = 0;
	args->hClass = NV01_EVENT_KERNEL_CALLBACK_EX;
	args->notifyIndex = NV01_EVENT_CLIENT_RM | id;
	args->data = NULL;

	ret = nvkm_gsp_rm_alloc_wr(&event->object, args);
	if (ret)
		return ret;

	event->device = device;
	event->id = id;

	ret = r535_gsp_device_event_get(event);
	if (ret) {
		nvkm_gsp_event_dtor(event);
		return ret;
	}

	mutex_lock(&gsp->client_id.mutex);
	event->func = func;
	list_add(&event->head, &client->events);
	mutex_unlock(&gsp->client_id.mutex);
	return 0;
}

static void
r535_gsp_device_dtor(struct nvkm_gsp_device *device)
{
	nvkm_gsp_rm_free(&device->subdevice);
	nvkm_gsp_rm_free(&device->object);
}

static int
r535_gsp_subdevice_ctor(struct nvkm_gsp_device *device)
{
	NV2080_ALLOC_PARAMETERS *args;

	return nvkm_gsp_rm_alloc(&device->object, 0x5d1d0000, NV20_SUBDEVICE_0, sizeof(*args),
				 &device->subdevice);
}

static int
r535_gsp_device_ctor(struct nvkm_gsp_client *client, struct nvkm_gsp_device *device)
{
	NV0080_ALLOC_PARAMETERS *args;
	int ret;

	args = nvkm_gsp_rm_alloc_get(&client->object, 0xde1d0000, NV01_DEVICE_0, sizeof(*args),
				     &device->object);
	if (IS_ERR(args))
		return PTR_ERR(args);

	args->hClientShare = client->object.handle;

	ret = nvkm_gsp_rm_alloc_wr(&device->object, args);
	if (ret)
		return ret;

	ret = r535_gsp_subdevice_ctor(device);
	if (ret)
		nvkm_gsp_rm_free(&device->object);

	return ret;
}

static void
r535_gsp_client_dtor(struct nvkm_gsp_client *client)
{
	struct nvkm_gsp *gsp = client->gsp;

	nvkm_gsp_rm_free(&client->object);

	mutex_lock(&gsp->client_id.mutex);
	idr_remove(&gsp->client_id.idr, client->object.handle & 0xffff);
	mutex_unlock(&gsp->client_id.mutex);

	client->gsp = NULL;
}

static int
r535_gsp_client_ctor(struct nvkm_gsp *gsp, struct nvkm_gsp_client *client)
{
	NV0000_ALLOC_PARAMETERS *args;
	int ret;

	mutex_lock(&gsp->client_id.mutex);
	ret = idr_alloc(&gsp->client_id.idr, client, 0, 0xffff + 1, GFP_KERNEL);
	mutex_unlock(&gsp->client_id.mutex);
	if (ret < 0)
		return ret;

	client->gsp = gsp;
	client->object.client = client;
	INIT_LIST_HEAD(&client->events);

	args = nvkm_gsp_rm_alloc_get(&client->object, 0xc1d00000 | ret, NV01_ROOT, sizeof(*args),
				     &client->object);
	if (IS_ERR(args)) {
		r535_gsp_client_dtor(client);
		return ret;
	}

	args->hClient = client->object.handle;
	args->processID = ~0;

	ret = nvkm_gsp_rm_alloc_wr(&client->object, args);
	if (ret) {
		r535_gsp_client_dtor(client);
		return ret;
	}

	return 0;
}

static int
r535_gsp_rpc_rm_free(struct nvkm_gsp_object *object)
{
	struct nvkm_gsp_client *client = object->client;
	struct nvkm_gsp *gsp = client->gsp;
	rpc_free_v03_00 *rpc;

	nvkm_debug(&gsp->subdev, "cli:0x%08x obj:0x%08x free\n",
		   client->object.handle, object->handle);

	rpc = nvkm_gsp_rpc_get(gsp, NV_VGPU_MSG_FUNCTION_FREE, sizeof(*rpc));
	if (WARN_ON(IS_ERR_OR_NULL(rpc)))
		return -EIO;

	rpc->params.hRoot = client->object.handle;
	rpc->params.hObjectParent = 0;
	rpc->params.hObjectOld = object->handle;
	return nvkm_gsp_rpc_wr(gsp, rpc, NVKM_GSP_RPC_REPLY_RECV);
}

static void
r535_gsp_rpc_rm_alloc_done(struct nvkm_gsp_object *object, void *params)
{
	rpc_gsp_rm_alloc_v03_00 *rpc = to_payload_hdr(params, rpc);

	nvkm_gsp_rpc_done(object->client->gsp, rpc);
}

static void *
r535_gsp_rpc_rm_alloc_push(struct nvkm_gsp_object *object, void *params)
{
	rpc_gsp_rm_alloc_v03_00 *rpc = to_payload_hdr(params, rpc);
	struct nvkm_gsp *gsp = object->client->gsp;
	void *ret = NULL;

	rpc = nvkm_gsp_rpc_push(gsp, rpc, NVKM_GSP_RPC_REPLY_RECV, sizeof(*rpc));
	if (IS_ERR_OR_NULL(rpc))
		return rpc;

	if (rpc->status) {
		ret = ERR_PTR(r535_rpc_status_to_errno(rpc->status));
		if (PTR_ERR(ret) != -EAGAIN && PTR_ERR(ret) != -EBUSY)
			nvkm_error(&gsp->subdev, "RM_ALLOC: 0x%x\n", rpc->status);
	}

	nvkm_gsp_rpc_done(gsp, rpc);

	return ret;
}

static void *
r535_gsp_rpc_rm_alloc_get(struct nvkm_gsp_object *object, u32 oclass,
			  u32 params_size)
{
	struct nvkm_gsp_client *client = object->client;
	struct nvkm_gsp *gsp = client->gsp;
	rpc_gsp_rm_alloc_v03_00 *rpc;

	nvkm_debug(&gsp->subdev, "cli:0x%08x obj:0x%08x new obj:0x%08x\n",
		   client->object.handle, object->parent->handle,
		   object->handle);

	nvkm_debug(&gsp->subdev, "cls:0x%08x params_size:%d\n", oclass,
		   params_size);

	rpc = nvkm_gsp_rpc_get(gsp, NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC,
			       sizeof(*rpc) + params_size);
	if (IS_ERR(rpc))
		return rpc;

	rpc->hClient = client->object.handle;
	rpc->hParent = object->parent->handle;
	rpc->hObject = object->handle;
	rpc->hClass = oclass;
	rpc->status = 0;
	rpc->paramsSize = params_size;
	return rpc->params;
}

static void
r535_gsp_rpc_rm_ctrl_done(struct nvkm_gsp_object *object, void *params)
{
	rpc_gsp_rm_control_v03_00 *rpc = to_payload_hdr(params, rpc);

	if (!params)
		return;
	nvkm_gsp_rpc_done(object->client->gsp, rpc);
}

static int
r535_gsp_rpc_rm_ctrl_push(struct nvkm_gsp_object *object, void **params, u32 repc)
{
	rpc_gsp_rm_control_v03_00 *rpc = to_payload_hdr((*params), rpc);
	struct nvkm_gsp *gsp = object->client->gsp;
	int ret = 0;

	rpc = nvkm_gsp_rpc_push(gsp, rpc, NVKM_GSP_RPC_REPLY_RECV, repc);
	if (IS_ERR_OR_NULL(rpc)) {
		*params = NULL;
		return PTR_ERR(rpc);
	}

	if (rpc->status) {
		ret = r535_rpc_status_to_errno(rpc->status);
		if (ret != -EAGAIN && ret != -EBUSY)
			nvkm_error(&gsp->subdev, "cli:0x%08x obj:0x%08x ctrl cmd:0x%08x failed: 0x%08x\n",
				   object->client->object.handle, object->handle, rpc->cmd, rpc->status);
	}

	if (repc)
		*params = rpc->params;
	else
		nvkm_gsp_rpc_done(gsp, rpc);

	return ret;
}

static void *
r535_gsp_rpc_rm_ctrl_get(struct nvkm_gsp_object *object, u32 cmd, u32 params_size)
{
	struct nvkm_gsp_client *client = object->client;
	struct nvkm_gsp *gsp = client->gsp;
	rpc_gsp_rm_control_v03_00 *rpc;

	nvkm_debug(&gsp->subdev, "cli:0x%08x obj:0x%08x ctrl cmd:0x%08x params_size:%d\n",
		   client->object.handle, object->handle, cmd, params_size);

	rpc = nvkm_gsp_rpc_get(gsp, NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL,
			       sizeof(*rpc) + params_size);
	if (IS_ERR(rpc))
		return rpc;

	rpc->hClient    = client->object.handle;
	rpc->hObject    = object->handle;
	rpc->cmd	= cmd;
	rpc->status     = 0;
	rpc->paramsSize = params_size;
	return rpc->params;
}

const struct nvkm_gsp_rm
r535_gsp_rm = {
	.api = &r535_rm,

	.rm_ctrl_get = r535_gsp_rpc_rm_ctrl_get,
	.rm_ctrl_push = r535_gsp_rpc_rm_ctrl_push,
	.rm_ctrl_done = r535_gsp_rpc_rm_ctrl_done,

	.rm_alloc_get = r535_gsp_rpc_rm_alloc_get,
	.rm_alloc_push = r535_gsp_rpc_rm_alloc_push,
	.rm_alloc_done = r535_gsp_rpc_rm_alloc_done,

	.rm_free = r535_gsp_rpc_rm_free,

	.client_ctor = r535_gsp_client_ctor,
	.client_dtor = r535_gsp_client_dtor,

	.device_ctor = r535_gsp_device_ctor,
	.device_dtor = r535_gsp_device_dtor,

	.event_ctor = r535_gsp_device_event_ctor,
	.event_dtor = r535_gsp_event_dtor,
};

static void
r535_gsp_msgq_work(struct work_struct *work)
{
	struct nvkm_gsp *gsp = container_of(work, typeof(*gsp), msgq.work);

	mutex_lock(&gsp->cmdq.mutex);
	if (*gsp->msgq.rptr != *gsp->msgq.wptr)
		r535_gsp_msg_recv(gsp, 0, 0);
	mutex_unlock(&gsp->cmdq.mutex);
}

static irqreturn_t
r535_gsp_intr(struct nvkm_inth *inth)
{
	struct nvkm_gsp *gsp = container_of(inth, typeof(*gsp), subdev.inth);
	struct nvkm_subdev *subdev = &gsp->subdev;
	u32 intr = nvkm_falcon_rd32(&gsp->falcon, 0x0008);
	u32 inte = nvkm_falcon_rd32(&gsp->falcon, gsp->falcon.func->addr2 +
						  gsp->falcon.func->riscv_irqmask);
	u32 stat = intr & inte;

	if (!stat) {
		nvkm_debug(subdev, "inte %08x %08x\n", intr, inte);
		return IRQ_NONE;
	}

	if (stat & 0x00000040) {
		nvkm_falcon_wr32(&gsp->falcon, 0x004, 0x00000040);
		schedule_work(&gsp->msgq.work);
		stat &= ~0x00000040;
	}

	if (stat) {
		nvkm_error(subdev, "intr %08x\n", stat);
		nvkm_falcon_wr32(&gsp->falcon, 0x014, stat);
		nvkm_falcon_wr32(&gsp->falcon, 0x004, stat);
	}

	nvkm_falcon_intr_retrigger(&gsp->falcon);
	return IRQ_HANDLED;
}

static int
r535_gsp_intr_get_table(struct nvkm_gsp *gsp)
{
	NV2080_CTRL_INTERNAL_INTR_GET_KERNEL_TABLE_PARAMS *ctrl;
	int ret = 0;

	ctrl = nvkm_gsp_rm_ctrl_get(&gsp->internal.device.subdevice,
				    NV2080_CTRL_CMD_INTERNAL_INTR_GET_KERNEL_TABLE, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ret = nvkm_gsp_rm_ctrl_push(&gsp->internal.device.subdevice, &ctrl, sizeof(*ctrl));
	if (WARN_ON(ret)) {
		nvkm_gsp_rm_ctrl_done(&gsp->internal.device.subdevice, ctrl);
		return ret;
	}

	for (unsigned i = 0; i < ctrl->tableLen; i++) {
		enum nvkm_subdev_type type;
		int inst;

		nvkm_debug(&gsp->subdev,
			   "%2d: engineIdx %3d pmcIntrMask %08x stall %08x nonStall %08x\n", i,
			   ctrl->table[i].engineIdx, ctrl->table[i].pmcIntrMask,
			   ctrl->table[i].vectorStall, ctrl->table[i].vectorNonStall);

		switch (ctrl->table[i].engineIdx) {
		case MC_ENGINE_IDX_GSP:
			type = NVKM_SUBDEV_GSP;
			inst = 0;
			break;
		case MC_ENGINE_IDX_DISP:
			type = NVKM_ENGINE_DISP;
			inst = 0;
			break;
		case MC_ENGINE_IDX_CE0 ... MC_ENGINE_IDX_CE9:
			type = NVKM_ENGINE_CE;
			inst = ctrl->table[i].engineIdx - MC_ENGINE_IDX_CE0;
			break;
		case MC_ENGINE_IDX_GR0:
			type = NVKM_ENGINE_GR;
			inst = 0;
			break;
		case MC_ENGINE_IDX_NVDEC0 ... MC_ENGINE_IDX_NVDEC7:
			type = NVKM_ENGINE_NVDEC;
			inst = ctrl->table[i].engineIdx - MC_ENGINE_IDX_NVDEC0;
			break;
		case MC_ENGINE_IDX_MSENC ... MC_ENGINE_IDX_MSENC2:
			type = NVKM_ENGINE_NVENC;
			inst = ctrl->table[i].engineIdx - MC_ENGINE_IDX_MSENC;
			break;
		case MC_ENGINE_IDX_NVJPEG0 ... MC_ENGINE_IDX_NVJPEG7:
			type = NVKM_ENGINE_NVJPG;
			inst = ctrl->table[i].engineIdx - MC_ENGINE_IDX_NVJPEG0;
			break;
		case MC_ENGINE_IDX_OFA0:
			type = NVKM_ENGINE_OFA;
			inst = 0;
			break;
		default:
			continue;
		}

		if (WARN_ON(gsp->intr_nr == ARRAY_SIZE(gsp->intr))) {
			ret = -ENOSPC;
			break;
		}

		gsp->intr[gsp->intr_nr].type = type;
		gsp->intr[gsp->intr_nr].inst = inst;
		gsp->intr[gsp->intr_nr].stall = ctrl->table[i].vectorStall;
		gsp->intr[gsp->intr_nr].nonstall = ctrl->table[i].vectorNonStall;
		gsp->intr_nr++;
	}

	nvkm_gsp_rm_ctrl_done(&gsp->internal.device.subdevice, ctrl);
	return ret;
}

static int
r535_gsp_rpc_get_gsp_static_info(struct nvkm_gsp *gsp)
{
	GspStaticConfigInfo *rpc;
	int last_usable = -1;

	rpc = nvkm_gsp_rpc_rd(gsp, NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO, sizeof(*rpc));
	if (IS_ERR(rpc))
		return PTR_ERR(rpc);

	gsp->internal.client.object.client = &gsp->internal.client;
	gsp->internal.client.object.parent = NULL;
	gsp->internal.client.object.handle = rpc->hInternalClient;
	gsp->internal.client.gsp = gsp;

	gsp->internal.device.object.client = &gsp->internal.client;
	gsp->internal.device.object.parent = &gsp->internal.client.object;
	gsp->internal.device.object.handle = rpc->hInternalDevice;

	gsp->internal.device.subdevice.client = &gsp->internal.client;
	gsp->internal.device.subdevice.parent = &gsp->internal.device.object;
	gsp->internal.device.subdevice.handle = rpc->hInternalSubdevice;

	gsp->bar.rm_bar1_pdb = rpc->bar1PdeBase;
	gsp->bar.rm_bar2_pdb = rpc->bar2PdeBase;

	for (int i = 0; i < rpc->fbRegionInfoParams.numFBRegions; i++) {
		NV2080_CTRL_CMD_FB_GET_FB_REGION_FB_REGION_INFO *reg =
			&rpc->fbRegionInfoParams.fbRegion[i];

		nvkm_debug(&gsp->subdev, "fb region %d: "
			   "%016llx-%016llx rsvd:%016llx perf:%08x comp:%d iso:%d prot:%d\n", i,
			   reg->base, reg->limit, reg->reserved, reg->performance,
			   reg->supportCompressed, reg->supportISO, reg->bProtected);

		if (!reg->reserved && !reg->bProtected) {
			if (reg->supportCompressed && reg->supportISO &&
			    !WARN_ON_ONCE(gsp->fb.region_nr >= ARRAY_SIZE(gsp->fb.region))) {
					const u64 size = (reg->limit + 1) - reg->base;

					gsp->fb.region[gsp->fb.region_nr].addr = reg->base;
					gsp->fb.region[gsp->fb.region_nr].size = size;
					gsp->fb.region_nr++;
			}

			last_usable = i;
		}
	}

	if (last_usable >= 0) {
		u32 rsvd_base = rpc->fbRegionInfoParams.fbRegion[last_usable].limit + 1;

		gsp->fb.rsvd_size = gsp->fb.heap.addr - rsvd_base;
	}

	for (int gpc = 0; gpc < ARRAY_SIZE(rpc->tpcInfo); gpc++) {
		if (rpc->gpcInfo.gpcMask & BIT(gpc)) {
			gsp->gr.tpcs += hweight32(rpc->tpcInfo[gpc].tpcMask);
			gsp->gr.gpcs++;
		}
	}

	nvkm_gsp_rpc_done(gsp, rpc);
	return 0;
}

static void
nvkm_gsp_mem_dtor(struct nvkm_gsp_mem *mem)
{
	if (mem->data) {
		/*
		 * Poison the buffer to catch any unexpected access from
		 * GSP-RM if the buffer was prematurely freed.
		 */
		memset(mem->data, 0xFF, mem->size);

		dma_free_coherent(mem->dev, mem->size, mem->data, mem->addr);
		put_device(mem->dev);

		memset(mem, 0, sizeof(*mem));
	}
}

/**
 * nvkm_gsp_mem_ctor - constructor for nvkm_gsp_mem objects
 * @gsp: gsp pointer
 * @size: number of bytes to allocate
 * @mem: nvkm_gsp_mem object to initialize
 *
 * Allocates a block of memory for use with GSP.
 *
 * This memory block can potentially out-live the driver's remove() callback,
 * so we take a device reference to ensure its lifetime. The reference is
 * dropped in the destructor.
 */
static int
nvkm_gsp_mem_ctor(struct nvkm_gsp *gsp, size_t size, struct nvkm_gsp_mem *mem)
{
	mem->data = dma_alloc_coherent(gsp->subdev.device->dev, size, &mem->addr, GFP_KERNEL);
	if (WARN_ON(!mem->data))
		return -ENOMEM;

	mem->size = size;
	mem->dev = get_device(gsp->subdev.device->dev);

	return 0;
}

static int
r535_gsp_postinit(struct nvkm_gsp *gsp)
{
	struct nvkm_device *device = gsp->subdev.device;
	int ret;

	ret = r535_gsp_rpc_get_gsp_static_info(gsp);
	if (WARN_ON(ret))
		return ret;

	INIT_WORK(&gsp->msgq.work, r535_gsp_msgq_work);

	ret = r535_gsp_intr_get_table(gsp);
	if (WARN_ON(ret))
		return ret;

	ret = nvkm_gsp_intr_stall(gsp, gsp->subdev.type, gsp->subdev.inst);
	if (WARN_ON(ret < 0))
		return ret;

	ret = nvkm_inth_add(&device->vfn->intr, ret, NVKM_INTR_PRIO_NORMAL, &gsp->subdev,
			    r535_gsp_intr, &gsp->subdev.inth);
	if (WARN_ON(ret))
		return ret;

	nvkm_inth_allow(&gsp->subdev.inth);
	nvkm_wr32(device, 0x110004, 0x00000040);

	/* Release the DMA buffers that were needed only for boot and init */
	nvkm_gsp_mem_dtor(&gsp->boot.fw);
	nvkm_gsp_mem_dtor(&gsp->libos);

	return ret;
}

static int
r535_gsp_rpc_unloading_guest_driver(struct nvkm_gsp *gsp, bool suspend)
{
	rpc_unloading_guest_driver_v1F_07 *rpc;

	rpc = nvkm_gsp_rpc_get(gsp, NV_VGPU_MSG_FUNCTION_UNLOADING_GUEST_DRIVER, sizeof(*rpc));
	if (IS_ERR(rpc))
		return PTR_ERR(rpc);

	if (suspend) {
		rpc->bInPMTransition = 1;
		rpc->bGc6Entering = 0;
		rpc->newLevel = NV2080_CTRL_GPU_SET_POWER_STATE_GPU_LEVEL_3;
	} else {
		rpc->bInPMTransition = 0;
		rpc->bGc6Entering = 0;
		rpc->newLevel = NV2080_CTRL_GPU_SET_POWER_STATE_GPU_LEVEL_0;
	}

	return nvkm_gsp_rpc_wr(gsp, rpc, NVKM_GSP_RPC_REPLY_RECV);
}

enum registry_type {
	REGISTRY_TABLE_ENTRY_TYPE_DWORD  = 1, /* 32-bit unsigned integer */
	REGISTRY_TABLE_ENTRY_TYPE_BINARY = 2, /* Binary blob */
	REGISTRY_TABLE_ENTRY_TYPE_STRING = 3, /* Null-terminated string */
};

/* An arbitrary limit to the length of a registry key */
#define REGISTRY_MAX_KEY_LENGTH		64

/**
 * struct registry_list_entry - linked list member for a registry key/value
 * @head: list_head struct
 * @type: dword, binary, or string
 * @klen: the length of name of the key
 * @vlen: the length of the value
 * @key: the key name
 * @dword: the data, if REGISTRY_TABLE_ENTRY_TYPE_DWORD
 * @binary: the data, if TYPE_BINARY or TYPE_STRING
 *
 * Every registry key/value is represented internally by this struct.
 *
 * Type DWORD is a simple 32-bit unsigned integer, and its value is stored in
 * @dword.
 *
 * Types BINARY and STRING are variable-length binary blobs.  The only real
 * difference between BINARY and STRING is that STRING is null-terminated and
 * is expected to contain only printable characters.
 *
 * Note: it is technically possible to have multiple keys with the same name
 * but different types, but this is not useful since GSP-RM expects keys to
 * have only one specific type.
 */
struct registry_list_entry {
	struct list_head head;
	enum registry_type type;
	size_t klen;
	char key[REGISTRY_MAX_KEY_LENGTH];
	size_t vlen;
	u32 dword;			/* TYPE_DWORD */
	u8 binary[] __counted_by(vlen);	/* TYPE_BINARY or TYPE_STRING */
};

/**
 * add_registry -- adds a registry entry
 * @gsp: gsp pointer
 * @key: name of the registry key
 * @type: type of data
 * @data: pointer to value
 * @length: size of data, in bytes
 *
 * Adds a registry key/value pair to the registry database.
 *
 * This function collects the registry information in a linked list.  After
 * all registry keys have been added, build_registry() is used to create the
 * RPC data structure.
 *
 * registry_rpc_size is a running total of the size of all registry keys.
 * It's used to avoid an O(n) calculation of the size when the RPC is built.
 *
 * Returns 0 on success, or negative error code on error.
 */
static int add_registry(struct nvkm_gsp *gsp, const char *key,
			enum registry_type type, const void *data, size_t length)
{
	struct registry_list_entry *reg;
	const size_t nlen = strnlen(key, REGISTRY_MAX_KEY_LENGTH) + 1;
	size_t alloc_size; /* extra bytes to alloc for binary or string value */

	if (nlen > REGISTRY_MAX_KEY_LENGTH)
		return -EINVAL;

	alloc_size = (type == REGISTRY_TABLE_ENTRY_TYPE_DWORD) ? 0 : length;

	reg = kmalloc(sizeof(*reg) + alloc_size, GFP_KERNEL);
	if (!reg)
		return -ENOMEM;

	switch (type) {
	case REGISTRY_TABLE_ENTRY_TYPE_DWORD:
		reg->dword = *(const u32 *)(data);
		break;
	case REGISTRY_TABLE_ENTRY_TYPE_BINARY:
	case REGISTRY_TABLE_ENTRY_TYPE_STRING:
		memcpy(reg->binary, data, alloc_size);
		break;
	default:
		nvkm_error(&gsp->subdev, "unrecognized registry type %u for '%s'\n",
			   type, key);
		kfree(reg);
		return -EINVAL;
	}

	memcpy(reg->key, key, nlen);
	reg->klen = nlen;
	reg->vlen = length;
	reg->type = type;

	list_add_tail(&reg->head, &gsp->registry_list);
	gsp->registry_rpc_size += sizeof(PACKED_REGISTRY_ENTRY) + nlen + alloc_size;

	return 0;
}

static int add_registry_num(struct nvkm_gsp *gsp, const char *key, u32 value)
{
	return add_registry(gsp, key, REGISTRY_TABLE_ENTRY_TYPE_DWORD,
			    &value, sizeof(u32));
}

static int add_registry_string(struct nvkm_gsp *gsp, const char *key, const char *value)
{
	return add_registry(gsp, key, REGISTRY_TABLE_ENTRY_TYPE_STRING,
			    value, strlen(value) + 1);
}

/**
 * build_registry -- create the registry RPC data
 * @gsp: gsp pointer
 * @registry: pointer to the RPC payload to fill
 *
 * After all registry key/value pairs have been added, call this function to
 * build the RPC.
 *
 * The registry RPC looks like this:
 *
 * +-----------------+
 * |NvU32 size;      |
 * |NvU32 numEntries;|
 * +-----------------+
 * +----------------------------------------+
 * |PACKED_REGISTRY_ENTRY                   |
 * +----------------------------------------+
 * |Null-terminated key (string) for entry 0|
 * +----------------------------------------+
 * |Binary/string data value for entry 0    | (only if necessary)
 * +----------------------------------------+
 *
 * +----------------------------------------+
 * |PACKED_REGISTRY_ENTRY                   |
 * +----------------------------------------+
 * |Null-terminated key (string) for entry 1|
 * +----------------------------------------+
 * |Binary/string data value for entry 1    | (only if necessary)
 * +----------------------------------------+
 * ... (and so on, one copy for each entry)
 *
 *
 * The 'data' field of an entry is either a 32-bit integer (for type DWORD)
 * or an offset into the PACKED_REGISTRY_TABLE (for types BINARY and STRING).
 *
 * All memory allocated by add_registry() is released.
 */
static void build_registry(struct nvkm_gsp *gsp, PACKED_REGISTRY_TABLE *registry)
{
	struct registry_list_entry *reg, *n;
	size_t str_offset;
	unsigned int i = 0;

	registry->numEntries = list_count_nodes(&gsp->registry_list);
	str_offset = struct_size(registry, entries, registry->numEntries);

	list_for_each_entry_safe(reg, n, &gsp->registry_list, head) {
		registry->entries[i].type = reg->type;
		registry->entries[i].length = reg->vlen;

		/* Append the key name to the table */
		registry->entries[i].nameOffset = str_offset;
		memcpy((void *)registry + str_offset, reg->key, reg->klen);
		str_offset += reg->klen;

		switch (reg->type) {
		case REGISTRY_TABLE_ENTRY_TYPE_DWORD:
			registry->entries[i].data = reg->dword;
			break;
		case REGISTRY_TABLE_ENTRY_TYPE_BINARY:
		case REGISTRY_TABLE_ENTRY_TYPE_STRING:
			/* If the type is binary or string, also append the value */
			memcpy((void *)registry + str_offset, reg->binary, reg->vlen);
			registry->entries[i].data = str_offset;
			str_offset += reg->vlen;
			break;
		default:
			break;
		}

		i++;
		list_del(&reg->head);
		kfree(reg);
	}

	/* Double-check that we calculated the sizes correctly */
	WARN_ON(gsp->registry_rpc_size != str_offset);

	registry->size = gsp->registry_rpc_size;
}

/**
 * clean_registry -- clean up registry memory in case of error
 * @gsp: gsp pointer
 *
 * Call this function to clean up all memory allocated by add_registry()
 * in case of error and build_registry() is not called.
 */
static void clean_registry(struct nvkm_gsp *gsp)
{
	struct registry_list_entry *reg, *n;

	list_for_each_entry_safe(reg, n, &gsp->registry_list, head) {
		list_del(&reg->head);
		kfree(reg);
	}

	gsp->registry_rpc_size = sizeof(PACKED_REGISTRY_TABLE);
}

MODULE_PARM_DESC(NVreg_RegistryDwords,
		 "A semicolon-separated list of key=integer pairs of GSP-RM registry keys");
static char *NVreg_RegistryDwords;
module_param(NVreg_RegistryDwords, charp, 0400);

/* dword only */
struct nv_gsp_registry_entries {
	const char *name;
	u32 value;
};

/*
 * r535_registry_entries - required registry entries for GSP-RM
 *
 * This array lists registry entries that are required for GSP-RM to
 * function correctly.
 *
 * RMSecBusResetEnable - enables PCI secondary bus reset
 * RMForcePcieConfigSave - forces GSP-RM to preserve PCI configuration
 *   registers on any PCI reset.
 */
static const struct nv_gsp_registry_entries r535_registry_entries[] = {
	{ "RMSecBusResetEnable", 1 },
	{ "RMForcePcieConfigSave", 1 },
};
#define NV_GSP_REG_NUM_ENTRIES ARRAY_SIZE(r535_registry_entries)

/**
 * strip - strips all characters in 'reject' from 's'
 * @s: string to strip
 * @reject: string of characters to remove
 *
 * 's' is modified.
 *
 * Returns the length of the new string.
 */
static size_t strip(char *s, const char *reject)
{
	char *p = s, *p2 = s;
	size_t length = 0;
	char c;

	do {
		while ((c = *p2) && strchr(reject, c))
			p2++;

		*p++ = c = *p2++;
		length++;
	} while (c);

	return length;
}

/**
 * r535_gsp_rpc_set_registry - build registry RPC and call GSP-RM
 * @gsp: gsp pointer
 *
 * The GSP-RM registry is a set of key/value pairs that configure some aspects
 * of GSP-RM. The keys are strings, and the values are 32-bit integers.
 *
 * The registry is built from a combination of a static hard-coded list (see
 * above) and entries passed on the driver's command line.
 */
static int
r535_gsp_rpc_set_registry(struct nvkm_gsp *gsp)
{
	PACKED_REGISTRY_TABLE *rpc;
	unsigned int i;
	int ret;

	INIT_LIST_HEAD(&gsp->registry_list);
	gsp->registry_rpc_size = sizeof(PACKED_REGISTRY_TABLE);

	for (i = 0; i < NV_GSP_REG_NUM_ENTRIES; i++) {
		ret = add_registry_num(gsp, r535_registry_entries[i].name,
				       r535_registry_entries[i].value);
		if (ret)
			goto fail;
	}

	/*
	 * The NVreg_RegistryDwords parameter is a string of key=value
	 * pairs separated by semicolons. We need to extract and trim each
	 * substring, and then parse the substring to extract the key and
	 * value.
	 */
	if (NVreg_RegistryDwords) {
		char *p = kstrdup(NVreg_RegistryDwords, GFP_KERNEL);
		char *start, *next = p, *equal;

		if (!p) {
			ret = -ENOMEM;
			goto fail;
		}

		/* Remove any whitespace from the parameter string */
		strip(p, " \t\n");

		while ((start = strsep(&next, ";"))) {
			long value;

			equal = strchr(start, '=');
			if (!equal || equal == start || equal[1] == 0) {
				nvkm_error(&gsp->subdev,
					   "ignoring invalid registry string '%s'\n",
					   start);
				continue;
			}

			/* Truncate the key=value string to just key */
			*equal = 0;

			ret = kstrtol(equal + 1, 0, &value);
			if (!ret) {
				ret = add_registry_num(gsp, start, value);
			} else {
				/* Not a number, so treat it as a string */
				ret = add_registry_string(gsp, start, equal + 1);
			}

			if (ret) {
				nvkm_error(&gsp->subdev,
					   "ignoring invalid registry key/value '%s=%s'\n",
					   start, equal + 1);
				continue;
			}
		}

		kfree(p);
	}

	rpc = nvkm_gsp_rpc_get(gsp, NV_VGPU_MSG_FUNCTION_SET_REGISTRY, gsp->registry_rpc_size);
	if (IS_ERR(rpc)) {
		ret = PTR_ERR(rpc);
		goto fail;
	}

	build_registry(gsp, rpc);

	return nvkm_gsp_rpc_wr(gsp, rpc, NVKM_GSP_RPC_REPLY_NOWAIT);

fail:
	clean_registry(gsp);
	return ret;
}

#if defined(CONFIG_ACPI) && defined(CONFIG_X86)
static void
r535_gsp_acpi_caps(acpi_handle handle, CAPS_METHOD_DATA *caps)
{
	const guid_t NVOP_DSM_GUID =
		GUID_INIT(0xA486D8F8, 0x0BDA, 0x471B,
			  0xA7, 0x2B, 0x60, 0x42, 0xA6, 0xB5, 0xBE, 0xE0);
	u64 NVOP_DSM_REV = 0x00000100;
	union acpi_object argv4 = {
		.buffer.type    = ACPI_TYPE_BUFFER,
		.buffer.length  = 4,
	}, *obj;

	caps->status = 0xffff;

	if (!acpi_check_dsm(handle, &NVOP_DSM_GUID, NVOP_DSM_REV, BIT_ULL(0x1a)))
		return;

	argv4.buffer.pointer = kmalloc(argv4.buffer.length, GFP_KERNEL);
	if (!argv4.buffer.pointer)
		return;

	obj = acpi_evaluate_dsm(handle, &NVOP_DSM_GUID, NVOP_DSM_REV, 0x1a, &argv4);
	if (!obj)
		goto done;

	if (WARN_ON(obj->type != ACPI_TYPE_BUFFER) ||
	    WARN_ON(obj->buffer.length != 4))
		goto done;

	caps->status = 0;
	caps->optimusCaps = *(u32 *)obj->buffer.pointer;

done:
	ACPI_FREE(obj);

	kfree(argv4.buffer.pointer);
}

static void
r535_gsp_acpi_jt(acpi_handle handle, JT_METHOD_DATA *jt)
{
	const guid_t JT_DSM_GUID =
		GUID_INIT(0xCBECA351L, 0x067B, 0x4924,
			  0x9C, 0xBD, 0xB4, 0x6B, 0x00, 0xB8, 0x6F, 0x34);
	u64 JT_DSM_REV = 0x00000103;
	u32 caps;
	union acpi_object argv4 = {
		.buffer.type    = ACPI_TYPE_BUFFER,
		.buffer.length  = sizeof(caps),
	}, *obj;

	jt->status = 0xffff;

	argv4.buffer.pointer = kmalloc(argv4.buffer.length, GFP_KERNEL);
	if (!argv4.buffer.pointer)
		return;

	obj = acpi_evaluate_dsm(handle, &JT_DSM_GUID, JT_DSM_REV, 0x1, &argv4);
	if (!obj)
		goto done;

	if (WARN_ON(obj->type != ACPI_TYPE_BUFFER) ||
	    WARN_ON(obj->buffer.length != 4))
		goto done;

	jt->status = 0;
	jt->jtCaps = *(u32 *)obj->buffer.pointer;
	jt->jtRevId = (jt->jtCaps & 0xfff00000) >> 20;
	jt->bSBIOSCaps = 0;

done:
	ACPI_FREE(obj);

	kfree(argv4.buffer.pointer);
}

static void
r535_gsp_acpi_mux_id(acpi_handle handle, u32 id, MUX_METHOD_DATA_ELEMENT *mode,
						 MUX_METHOD_DATA_ELEMENT *part)
{
	union acpi_object mux_arg = { ACPI_TYPE_INTEGER };
	struct acpi_object_list input = { 1, &mux_arg };
	acpi_handle iter = NULL, handle_mux = NULL;
	acpi_status status;
	unsigned long long value;

	mode->status = 0xffff;
	part->status = 0xffff;

	do {
		status = acpi_get_next_object(ACPI_TYPE_DEVICE, handle, iter, &iter);
		if (ACPI_FAILURE(status) || !iter)
			return;

		status = acpi_evaluate_integer(iter, "_ADR", NULL, &value);
		if (ACPI_FAILURE(status) || value != id)
			continue;

		handle_mux = iter;
	} while (!handle_mux);

	if (!handle_mux)
		return;

	/* I -think- 0 means "acquire" according to nvidia's driver source */
	input.pointer->integer.type = ACPI_TYPE_INTEGER;
	input.pointer->integer.value = 0;

	status = acpi_evaluate_integer(handle_mux, "MXDM", &input, &value);
	if (ACPI_SUCCESS(status)) {
		mode->acpiId = id;
		mode->mode   = value;
		mode->status = 0;
	}

	status = acpi_evaluate_integer(handle_mux, "MXDS", &input, &value);
	if (ACPI_SUCCESS(status)) {
		part->acpiId = id;
		part->mode   = value;
		part->status = 0;
	}
}

static void
r535_gsp_acpi_mux(acpi_handle handle, DOD_METHOD_DATA *dod, MUX_METHOD_DATA *mux)
{
	mux->tableLen = dod->acpiIdListLen / sizeof(dod->acpiIdList[0]);

	for (int i = 0; i < mux->tableLen; i++) {
		r535_gsp_acpi_mux_id(handle, dod->acpiIdList[i], &mux->acpiIdMuxModeTable[i],
								 &mux->acpiIdMuxPartTable[i]);
	}
}

static void
r535_gsp_acpi_dod(acpi_handle handle, DOD_METHOD_DATA *dod)
{
	acpi_status status;
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *_DOD;

	dod->status = 0xffff;

	status = acpi_evaluate_object(handle, "_DOD", NULL, &output);
	if (ACPI_FAILURE(status))
		return;

	_DOD = output.pointer;

	if (WARN_ON(_DOD->type != ACPI_TYPE_PACKAGE) ||
	    WARN_ON(_DOD->package.count > ARRAY_SIZE(dod->acpiIdList)))
		return;

	for (int i = 0; i < _DOD->package.count; i++) {
		if (WARN_ON(_DOD->package.elements[i].type != ACPI_TYPE_INTEGER))
			return;

		dod->acpiIdList[i] = _DOD->package.elements[i].integer.value;
		dod->acpiIdListLen += sizeof(dod->acpiIdList[0]);
	}

	dod->status = 0;
	kfree(output.pointer);
}
#endif

static void
r535_gsp_acpi_info(struct nvkm_gsp *gsp, ACPI_METHOD_DATA *acpi)
{
#if defined(CONFIG_ACPI) && defined(CONFIG_X86)
	acpi_handle handle = ACPI_HANDLE(gsp->subdev.device->dev);

	if (!handle)
		return;

	acpi->bValid = 1;

	r535_gsp_acpi_dod(handle, &acpi->dodMethodData);
	if (acpi->dodMethodData.status == 0)
		r535_gsp_acpi_mux(handle, &acpi->dodMethodData, &acpi->muxMethodData);

	r535_gsp_acpi_jt(handle, &acpi->jtMethodData);
	r535_gsp_acpi_caps(handle, &acpi->capsMethodData);
#endif
}

static int
r535_gsp_rpc_set_system_info(struct nvkm_gsp *gsp)
{
	struct nvkm_device *device = gsp->subdev.device;
	struct nvkm_device_pci *pdev = container_of(device, typeof(*pdev), device);
	GspSystemInfo *info;

	if (WARN_ON(device->type == NVKM_DEVICE_TEGRA))
		return -ENOSYS;

	info = nvkm_gsp_rpc_get(gsp, NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO, sizeof(*info));
	if (IS_ERR(info))
		return PTR_ERR(info);

	info->gpuPhysAddr = device->func->resource_addr(device, 0);
	info->gpuPhysFbAddr = device->func->resource_addr(device, 1);
	info->gpuPhysInstAddr = device->func->resource_addr(device, 3);
	info->nvDomainBusDeviceFunc = pci_dev_id(pdev->pdev);
	info->maxUserVa = TASK_SIZE;
	info->pciConfigMirrorBase = 0x088000;
	info->pciConfigMirrorSize = 0x001000;
	r535_gsp_acpi_info(gsp, &info->acpiMethodData);

	return nvkm_gsp_rpc_wr(gsp, info, NVKM_GSP_RPC_REPLY_NOWAIT);
}

static int
r535_gsp_msg_os_error_log(void *priv, u32 fn, void *repv, u32 repc)
{
	struct nvkm_gsp *gsp = priv;
	struct nvkm_subdev *subdev = &gsp->subdev;
	rpc_os_error_log_v17_00 *msg = repv;

	if (WARN_ON(repc < sizeof(*msg)))
		return -EINVAL;

	nvkm_error(subdev, "Xid:%d %s\n", msg->exceptType, msg->errString);
	return 0;
}

static int
r535_gsp_msg_rc_triggered(void *priv, u32 fn, void *repv, u32 repc)
{
	rpc_rc_triggered_v17_02 *msg = repv;
	struct nvkm_gsp *gsp = priv;
	struct nvkm_subdev *subdev = &gsp->subdev;
	struct nvkm_chan *chan;
	unsigned long flags;

	if (WARN_ON(repc < sizeof(*msg)))
		return -EINVAL;

	nvkm_error(subdev, "rc engn:%08x chid:%d type:%d scope:%d part:%d\n",
		   msg->nv2080EngineType, msg->chid, msg->exceptType, msg->scope,
		   msg->partitionAttributionId);

	chan = nvkm_chan_get_chid(&subdev->device->fifo->engine, msg->chid / 8, &flags);
	if (!chan) {
		nvkm_error(subdev, "rc chid:%d not found!\n", msg->chid);
		return 0;
	}

	nvkm_chan_error(chan, false);
	nvkm_chan_put(&chan, flags);
	return 0;
}

static int
r535_gsp_msg_mmu_fault_queued(void *priv, u32 fn, void *repv, u32 repc)
{
	struct nvkm_gsp *gsp = priv;
	struct nvkm_subdev *subdev = &gsp->subdev;

	WARN_ON(repc != 0);

	nvkm_error(subdev, "mmu fault queued\n");
	return 0;
}

static int
r535_gsp_msg_post_event(void *priv, u32 fn, void *repv, u32 repc)
{
	struct nvkm_gsp *gsp = priv;
	struct nvkm_gsp_client *client;
	struct nvkm_subdev *subdev = &gsp->subdev;
	rpc_post_event_v17_00 *msg = repv;

	if (WARN_ON(repc < sizeof(*msg)))
		return -EINVAL;
	if (WARN_ON(repc != sizeof(*msg) + msg->eventDataSize))
		return -EINVAL;

	nvkm_debug(subdev, "event: %08x %08x %d %08x %08x %d %d\n",
		   msg->hClient, msg->hEvent, msg->notifyIndex, msg->data,
		   msg->status, msg->eventDataSize, msg->bNotifyList);

	mutex_lock(&gsp->client_id.mutex);
	client = idr_find(&gsp->client_id.idr, msg->hClient & 0xffff);
	if (client) {
		struct nvkm_gsp_event *event;
		bool handled = false;

		list_for_each_entry(event, &client->events, head) {
			if (event->object.handle == msg->hEvent) {
				event->func(event, msg->eventData, msg->eventDataSize);
				handled = true;
			}
		}

		if (!handled) {
			nvkm_error(subdev, "event: cid 0x%08x event 0x%08x not found!\n",
				   msg->hClient, msg->hEvent);
		}
	} else {
		nvkm_error(subdev, "event: cid 0x%08x not found!\n", msg->hClient);
	}
	mutex_unlock(&gsp->client_id.mutex);
	return 0;
}

/**
 * r535_gsp_msg_run_cpu_sequencer() -- process I/O commands from the GSP
 * @priv: gsp pointer
 * @fn: function number (ignored)
 * @repv: pointer to libos print RPC
 * @repc: message size
 *
 * The GSP sequencer is a list of I/O commands that the GSP can send to
 * the driver to perform for various purposes.  The most common usage is to
 * perform a special mid-initialization reset.
 */
static int
r535_gsp_msg_run_cpu_sequencer(void *priv, u32 fn, void *repv, u32 repc)
{
	struct nvkm_gsp *gsp = priv;
	struct nvkm_subdev *subdev = &gsp->subdev;
	struct nvkm_device *device = subdev->device;
	rpc_run_cpu_sequencer_v17_00 *seq = repv;
	int ptr = 0, ret;

	nvkm_debug(subdev, "seq: %08x %08x\n", seq->bufferSizeDWord, seq->cmdIndex);

	while (ptr < seq->cmdIndex) {
		GSP_SEQUENCER_BUFFER_CMD *cmd = (void *)&seq->commandBuffer[ptr];

		ptr += 1;
		ptr += GSP_SEQUENCER_PAYLOAD_SIZE_DWORDS(cmd->opCode);

		switch (cmd->opCode) {
		case GSP_SEQ_BUF_OPCODE_REG_WRITE: {
			u32 addr = cmd->payload.regWrite.addr;
			u32 data = cmd->payload.regWrite.val;

			nvkm_trace(subdev, "seq wr32 %06x %08x\n", addr, data);
			nvkm_wr32(device, addr, data);
		}
			break;
		case GSP_SEQ_BUF_OPCODE_REG_MODIFY: {
			u32 addr = cmd->payload.regModify.addr;
			u32 mask = cmd->payload.regModify.mask;
			u32 data = cmd->payload.regModify.val;

			nvkm_trace(subdev, "seq mask %06x %08x %08x\n", addr, mask, data);
			nvkm_mask(device, addr, mask, data);
		}
			break;
		case GSP_SEQ_BUF_OPCODE_REG_POLL: {
			u32 addr = cmd->payload.regPoll.addr;
			u32 mask = cmd->payload.regPoll.mask;
			u32 data = cmd->payload.regPoll.val;
			u32 usec = cmd->payload.regPoll.timeout ?: 4000000;
			//u32 error = cmd->payload.regPoll.error;

			nvkm_trace(subdev, "seq poll %06x %08x %08x %d\n", addr, mask, data, usec);
			nvkm_rd32(device, addr);
			nvkm_usec(device, usec,
				if ((nvkm_rd32(device, addr) & mask) == data)
					break;
			);
		}
			break;
		case GSP_SEQ_BUF_OPCODE_DELAY_US: {
			u32 usec = cmd->payload.delayUs.val;

			nvkm_trace(subdev, "seq usec %d\n", usec);
			udelay(usec);
		}
			break;
		case GSP_SEQ_BUF_OPCODE_REG_STORE: {
			u32 addr = cmd->payload.regStore.addr;
			u32 slot = cmd->payload.regStore.index;

			seq->regSaveArea[slot] = nvkm_rd32(device, addr);
			nvkm_trace(subdev, "seq save %08x -> %d: %08x\n", addr, slot,
				   seq->regSaveArea[slot]);
		}
			break;
		case GSP_SEQ_BUF_OPCODE_CORE_RESET:
			nvkm_trace(subdev, "seq core reset\n");
			nvkm_falcon_reset(&gsp->falcon);
			nvkm_falcon_mask(&gsp->falcon, 0x624, 0x00000080, 0x00000080);
			nvkm_falcon_wr32(&gsp->falcon, 0x10c, 0x00000000);
			break;
		case GSP_SEQ_BUF_OPCODE_CORE_START:
			nvkm_trace(subdev, "seq core start\n");
			if (nvkm_falcon_rd32(&gsp->falcon, 0x100) & 0x00000040)
				nvkm_falcon_wr32(&gsp->falcon, 0x130, 0x00000002);
			else
				nvkm_falcon_wr32(&gsp->falcon, 0x100, 0x00000002);
			break;
		case GSP_SEQ_BUF_OPCODE_CORE_WAIT_FOR_HALT:
			nvkm_trace(subdev, "seq core wait halt\n");
			nvkm_msec(device, 2000,
				if (nvkm_falcon_rd32(&gsp->falcon, 0x100) & 0x00000010)
					break;
			);
			break;
		case GSP_SEQ_BUF_OPCODE_CORE_RESUME: {
			struct nvkm_sec2 *sec2 = device->sec2;
			u32 mbox0;

			nvkm_trace(subdev, "seq core resume\n");

			ret = gsp->func->reset(gsp);
			if (WARN_ON(ret))
				return ret;

			nvkm_falcon_wr32(&gsp->falcon, 0x040, lower_32_bits(gsp->libos.addr));
			nvkm_falcon_wr32(&gsp->falcon, 0x044, upper_32_bits(gsp->libos.addr));

			nvkm_falcon_start(&sec2->falcon);

			if (nvkm_msec(device, 2000,
				if (nvkm_rd32(device, 0x1180f8) & 0x04000000)
					break;
			) < 0)
				return -ETIMEDOUT;

			mbox0 = nvkm_falcon_rd32(&sec2->falcon, 0x040);
			if (WARN_ON(mbox0)) {
				nvkm_error(&gsp->subdev, "seq core resume sec2: 0x%x\n", mbox0);
				return -EIO;
			}

			nvkm_falcon_wr32(&gsp->falcon, 0x080, gsp->boot.app_version);

			if (WARN_ON(!nvkm_falcon_riscv_active(&gsp->falcon)))
				return -EIO;
		}
			break;
		default:
			nvkm_error(subdev, "unknown sequencer opcode %08x\n", cmd->opCode);
			return -EINVAL;
		}
	}

	return 0;
}

static int
r535_gsp_booter_unload(struct nvkm_gsp *gsp, u32 mbox0, u32 mbox1)
{
	struct nvkm_subdev *subdev = &gsp->subdev;
	struct nvkm_device *device = subdev->device;
	u32 wpr2_hi;
	int ret;

	wpr2_hi = nvkm_rd32(device, 0x1fa828);
	if (!wpr2_hi) {
		nvkm_debug(subdev, "WPR2 not set - skipping booter unload\n");
		return 0;
	}

	ret = nvkm_falcon_fw_boot(&gsp->booter.unload, &gsp->subdev, true, &mbox0, &mbox1, 0, 0);
	if (WARN_ON(ret))
		return ret;

	wpr2_hi = nvkm_rd32(device, 0x1fa828);
	if (WARN_ON(wpr2_hi))
		return -EIO;

	return 0;
}

static int
r535_gsp_booter_load(struct nvkm_gsp *gsp, u32 mbox0, u32 mbox1)
{
	int ret;

	ret = nvkm_falcon_fw_boot(&gsp->booter.load, &gsp->subdev, true, &mbox0, &mbox1, 0, 0);
	if (ret)
		return ret;

	nvkm_falcon_wr32(&gsp->falcon, 0x080, gsp->boot.app_version);

	if (WARN_ON(!nvkm_falcon_riscv_active(&gsp->falcon)))
		return -EIO;

	return 0;
}

static int
r535_gsp_wpr_meta_init(struct nvkm_gsp *gsp)
{
	GspFwWprMeta *meta;
	int ret;

	ret = nvkm_gsp_mem_ctor(gsp, 0x1000, &gsp->wpr_meta);
	if (ret)
		return ret;

	meta = gsp->wpr_meta.data;

	meta->magic = GSP_FW_WPR_META_MAGIC;
	meta->revision = GSP_FW_WPR_META_REVISION;

	meta->sysmemAddrOfRadix3Elf = gsp->radix3.lvl0.addr;
	meta->sizeOfRadix3Elf = gsp->fb.wpr2.elf.size;

	meta->sysmemAddrOfBootloader = gsp->boot.fw.addr;
	meta->sizeOfBootloader = gsp->boot.fw.size;
	meta->bootloaderCodeOffset = gsp->boot.code_offset;
	meta->bootloaderDataOffset = gsp->boot.data_offset;
	meta->bootloaderManifestOffset = gsp->boot.manifest_offset;

	meta->sysmemAddrOfSignature = gsp->sig.addr;
	meta->sizeOfSignature = gsp->sig.size;

	meta->gspFwRsvdStart = gsp->fb.heap.addr;
	meta->nonWprHeapOffset = gsp->fb.heap.addr;
	meta->nonWprHeapSize = gsp->fb.heap.size;
	meta->gspFwWprStart = gsp->fb.wpr2.addr;
	meta->gspFwHeapOffset = gsp->fb.wpr2.heap.addr;
	meta->gspFwHeapSize = gsp->fb.wpr2.heap.size;
	meta->gspFwOffset = gsp->fb.wpr2.elf.addr;
	meta->bootBinOffset = gsp->fb.wpr2.boot.addr;
	meta->frtsOffset = gsp->fb.wpr2.frts.addr;
	meta->frtsSize = gsp->fb.wpr2.frts.size;
	meta->gspFwWprEnd = ALIGN_DOWN(gsp->fb.bios.vga_workspace.addr, 0x20000);
	meta->fbSize = gsp->fb.size;
	meta->vgaWorkspaceOffset = gsp->fb.bios.vga_workspace.addr;
	meta->vgaWorkspaceSize = gsp->fb.bios.vga_workspace.size;
	meta->bootCount = 0;
	meta->partitionRpcAddr = 0;
	meta->partitionRpcRequestOffset = 0;
	meta->partitionRpcReplyOffset = 0;
	meta->verified = 0;
	return 0;
}

static int
r535_gsp_shared_init(struct nvkm_gsp *gsp)
{
	struct {
		msgqTxHeader tx;
		msgqRxHeader rx;
	} *cmdq, *msgq;
	int ret, i;

	gsp->shm.cmdq.size = 0x40000;
	gsp->shm.msgq.size = 0x40000;

	gsp->shm.ptes.nr  = (gsp->shm.cmdq.size + gsp->shm.msgq.size) >> GSP_PAGE_SHIFT;
	gsp->shm.ptes.nr += DIV_ROUND_UP(gsp->shm.ptes.nr * sizeof(u64), GSP_PAGE_SIZE);
	gsp->shm.ptes.size = ALIGN(gsp->shm.ptes.nr * sizeof(u64), GSP_PAGE_SIZE);

	ret = nvkm_gsp_mem_ctor(gsp, gsp->shm.ptes.size +
				     gsp->shm.cmdq.size +
				     gsp->shm.msgq.size,
				&gsp->shm.mem);
	if (ret)
		return ret;

	gsp->shm.ptes.ptr = gsp->shm.mem.data;
	gsp->shm.cmdq.ptr = (u8 *)gsp->shm.ptes.ptr + gsp->shm.ptes.size;
	gsp->shm.msgq.ptr = (u8 *)gsp->shm.cmdq.ptr + gsp->shm.cmdq.size;

	for (i = 0; i < gsp->shm.ptes.nr; i++)
		gsp->shm.ptes.ptr[i] = gsp->shm.mem.addr + (i << GSP_PAGE_SHIFT);

	cmdq = gsp->shm.cmdq.ptr;
	cmdq->tx.version = 0;
	cmdq->tx.size = gsp->shm.cmdq.size;
	cmdq->tx.entryOff = GSP_PAGE_SIZE;
	cmdq->tx.msgSize = GSP_PAGE_SIZE;
	cmdq->tx.msgCount = (cmdq->tx.size - cmdq->tx.entryOff) / cmdq->tx.msgSize;
	cmdq->tx.writePtr = 0;
	cmdq->tx.flags = 1;
	cmdq->tx.rxHdrOff = offsetof(typeof(*cmdq), rx.readPtr);

	msgq = gsp->shm.msgq.ptr;

	gsp->cmdq.cnt = cmdq->tx.msgCount;
	gsp->cmdq.wptr = &cmdq->tx.writePtr;
	gsp->cmdq.rptr = &msgq->rx.readPtr;
	gsp->msgq.cnt = cmdq->tx.msgCount;
	gsp->msgq.wptr = &msgq->tx.writePtr;
	gsp->msgq.rptr = &cmdq->rx.readPtr;
	return 0;
}

static int
r535_gsp_rmargs_init(struct nvkm_gsp *gsp, bool resume)
{
	GSP_ARGUMENTS_CACHED *args;
	int ret;

	if (!resume) {
		ret = r535_gsp_shared_init(gsp);
		if (ret)
			return ret;

		ret = nvkm_gsp_mem_ctor(gsp, 0x1000, &gsp->rmargs);
		if (ret)
			return ret;
	}

	args = gsp->rmargs.data;
	args->messageQueueInitArguments.sharedMemPhysAddr = gsp->shm.mem.addr;
	args->messageQueueInitArguments.pageTableEntryCount = gsp->shm.ptes.nr;
	args->messageQueueInitArguments.cmdQueueOffset =
		(u8 *)gsp->shm.cmdq.ptr - (u8 *)gsp->shm.mem.data;
	args->messageQueueInitArguments.statQueueOffset =
		(u8 *)gsp->shm.msgq.ptr - (u8 *)gsp->shm.mem.data;

	if (!resume) {
		args->srInitArguments.oldLevel = 0;
		args->srInitArguments.flags = 0;
		args->srInitArguments.bInPMTransition = 0;
	} else {
		args->srInitArguments.oldLevel = NV2080_CTRL_GPU_SET_POWER_STATE_GPU_LEVEL_3;
		args->srInitArguments.flags = 0;
		args->srInitArguments.bInPMTransition = 1;
	}

	return 0;
}

#ifdef CONFIG_DEBUG_FS

/*
 * If GSP-RM load fails, then the GSP nvkm object will be deleted, the logging
 * debugfs entries will be deleted, and it will not be possible to debug the
 * load failure. The keep_gsp_logging parameter tells Nouveau to copy the
 * logging buffers to new debugfs entries, and these entries are retained
 * until the driver unloads.
 */
static bool keep_gsp_logging;
module_param(keep_gsp_logging, bool, 0444);
MODULE_PARM_DESC(keep_gsp_logging,
		 "Migrate the GSP-RM logging debugfs entries upon exit");

/*
 * GSP-RM uses a pseudo-class mechanism to define of a variety of per-"engine"
 * data structures, and each engine has a "class ID" genererated by a
 * pre-processor. This is the class ID for the PMU.
 */
#define NV_GSP_MSG_EVENT_UCODE_LIBOS_CLASS_PMU		0xf3d722

/**
 * struct rpc_ucode_libos_print_v1e_08 - RPC payload for libos print buffers
 * @ucode_eng_desc: the engine descriptor
 * @libos_print_buf_size: the size of the libos_print_buf[]
 * @libos_print_buf: the actual buffer
 *
 * The engine descriptor is divided into 31:8 "class ID" and 7:0 "instance
 * ID". We only care about messages from PMU.
 */
struct rpc_ucode_libos_print_v1e_08 {
	u32 ucode_eng_desc;
	u32 libos_print_buf_size;
	u8 libos_print_buf[];
};

/**
 * r535_gsp_msg_libos_print - capture log message from the PMU
 * @priv: gsp pointer
 * @fn: function number (ignored)
 * @repv: pointer to libos print RPC
 * @repc: message size
 *
 * Called when we receive a UCODE_LIBOS_PRINT event RPC from GSP-RM. This RPC
 * contains the contents of the libos print buffer from PMU. It is typically
 * only written to when PMU encounters an error.
 *
 * Technically this RPC can be used to pass print buffers from any number of
 * GSP-RM engines, but we only expect to receive them for the PMU.
 *
 * For the PMU, the buffer is 4K in size and the RPC always contains the full
 * contents.
 */
static int
r535_gsp_msg_libos_print(void *priv, u32 fn, void *repv, u32 repc)
{
	struct nvkm_gsp *gsp = priv;
	struct nvkm_subdev *subdev = &gsp->subdev;
	struct rpc_ucode_libos_print_v1e_08 *rpc = repv;
	unsigned int class = rpc->ucode_eng_desc >> 8;

	nvkm_debug(subdev, "received libos print from class 0x%x for %u bytes\n",
		   class, rpc->libos_print_buf_size);

	if (class != NV_GSP_MSG_EVENT_UCODE_LIBOS_CLASS_PMU) {
		nvkm_warn(subdev,
			  "received libos print from unknown class 0x%x\n",
			  class);
		return -ENOMSG;
	}

	if (rpc->libos_print_buf_size > GSP_PAGE_SIZE) {
		nvkm_error(subdev, "libos print is too large (%u bytes)\n",
			   rpc->libos_print_buf_size);
		return -E2BIG;
	}

	memcpy(gsp->blob_pmu.data, rpc->libos_print_buf, rpc->libos_print_buf_size);

	return 0;
}

/**
 * create_debugfs - create a blob debugfs entry
 * @gsp: gsp pointer
 * @name: name of this dentry
 * @blob: blob wrapper
 *
 * Creates a debugfs entry for a logging buffer with the name 'name'.
 */
static struct dentry *create_debugfs(struct nvkm_gsp *gsp, const char *name,
				     struct debugfs_blob_wrapper *blob)
{
	struct dentry *dent;

	dent = debugfs_create_blob(name, 0444, gsp->debugfs.parent, blob);
	if (IS_ERR(dent)) {
		nvkm_error(&gsp->subdev,
			   "failed to create %s debugfs entry\n", name);
		return NULL;
	}

	/*
	 * For some reason, debugfs_create_blob doesn't set the size of the
	 * dentry, so do that here.  See [1]
	 *
	 * [1] https://lore.kernel.org/r/linux-fsdevel/20240207200619.3354549-1-ttabi@nvidia.com/
	 */
	i_size_write(d_inode(dent), blob->size);

	return dent;
}

/**
 * r535_gsp_libos_debugfs_init - create logging debugfs entries
 * @gsp: gsp pointer
 *
 * Create the debugfs entries. This exposes the log buffers to userspace so
 * that an external tool can parse it.
 *
 * The 'logpmu' contains exception dumps from the PMU. It is written via an
 * RPC sent from GSP-RM and must be only 4KB. We create it here because it's
 * only useful if there is a debugfs entry to expose it. If we get the PMU
 * logging RPC and there is no debugfs entry, the RPC is just ignored.
 *
 * The blob_init, blob_rm, and blob_pmu objects can't be transient
 * because debugfs_create_blob doesn't copy them.
 *
 * NOTE: OpenRM loads the logging elf image and prints the log messages
 * in real-time. We may add that capability in the future, but that
 * requires loading ELF images that are not distributed with the driver and
 * adding the parsing code to Nouveau.
 *
 * Ideally, this should be part of nouveau_debugfs_init(), but that function
 * is called too late. We really want to create these debugfs entries before
 * r535_gsp_booter_load() is called, so that if GSP-RM fails to initialize,
 * there could still be a log to capture.
 */
static void
r535_gsp_libos_debugfs_init(struct nvkm_gsp *gsp)
{
	struct device *dev = gsp->subdev.device->dev;

	/* Create a new debugfs directory with a name unique to this GPU. */
	gsp->debugfs.parent = debugfs_create_dir(dev_name(dev), nouveau_debugfs_root);
	if (IS_ERR(gsp->debugfs.parent)) {
		nvkm_error(&gsp->subdev,
			   "failed to create %s debugfs root\n", dev_name(dev));
		return;
	}

	gsp->blob_init.data = gsp->loginit.data;
	gsp->blob_init.size = gsp->loginit.size;
	gsp->blob_intr.data = gsp->logintr.data;
	gsp->blob_intr.size = gsp->logintr.size;
	gsp->blob_rm.data = gsp->logrm.data;
	gsp->blob_rm.size = gsp->logrm.size;

	gsp->debugfs.init = create_debugfs(gsp, "loginit", &gsp->blob_init);
	if (!gsp->debugfs.init)
		goto error;

	gsp->debugfs.intr = create_debugfs(gsp, "logintr", &gsp->blob_intr);
	if (!gsp->debugfs.intr)
		goto error;

	gsp->debugfs.rm = create_debugfs(gsp, "logrm", &gsp->blob_rm);
	if (!gsp->debugfs.rm)
		goto error;

	/*
	 * Since the PMU buffer is copied from an RPC, it doesn't need to be
	 * a DMA buffer.
	 */
	gsp->blob_pmu.size = GSP_PAGE_SIZE;
	gsp->blob_pmu.data = kzalloc(gsp->blob_pmu.size, GFP_KERNEL);
	if (!gsp->blob_pmu.data)
		goto error;

	gsp->debugfs.pmu = create_debugfs(gsp, "logpmu", &gsp->blob_pmu);
	if (!gsp->debugfs.pmu) {
		kfree(gsp->blob_pmu.data);
		goto error;
	}

	i_size_write(d_inode(gsp->debugfs.init), gsp->blob_init.size);
	i_size_write(d_inode(gsp->debugfs.intr), gsp->blob_intr.size);
	i_size_write(d_inode(gsp->debugfs.rm), gsp->blob_rm.size);
	i_size_write(d_inode(gsp->debugfs.pmu), gsp->blob_pmu.size);

	r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_UCODE_LIBOS_PRINT,
			      r535_gsp_msg_libos_print, gsp);

	nvkm_debug(&gsp->subdev, "created debugfs GSP-RM logging entries\n");

	if (keep_gsp_logging) {
		nvkm_info(&gsp->subdev,
			  "logging buffers will be retained on failure\n");
	}

	return;

error:
	debugfs_remove(gsp->debugfs.parent);
	gsp->debugfs.parent = NULL;
}

#endif

static inline u64
r535_gsp_libos_id8(const char *name)
{
	u64 id = 0;

	for (int i = 0; i < sizeof(id) && *name; i++, name++)
		id = (id << 8) | *name;

	return id;
}

/**
 * create_pte_array() - creates a PTE array of a physically contiguous buffer
 * @ptes: pointer to the array
 * @addr: base address of physically contiguous buffer (GSP_PAGE_SIZE aligned)
 * @size: size of the buffer
 *
 * GSP-RM sometimes expects physically-contiguous buffers to have an array of
 * "PTEs" for each page in that buffer.  Although in theory that allows for
 * the buffer to be physically discontiguous, GSP-RM does not currently
 * support that.
 *
 * In this case, the PTEs are DMA addresses of each page of the buffer.  Since
 * the buffer is physically contiguous, calculating all the PTEs is simple
 * math.
 *
 * See memdescGetPhysAddrsForGpu()
 */
static void create_pte_array(u64 *ptes, dma_addr_t addr, size_t size)
{
	unsigned int num_pages = DIV_ROUND_UP_ULL(size, GSP_PAGE_SIZE);
	unsigned int i;

	for (i = 0; i < num_pages; i++)
		ptes[i] = (u64)addr + (i << GSP_PAGE_SHIFT);
}

/**
 * r535_gsp_libos_init() -- create the libos arguments structure
 * @gsp: gsp pointer
 *
 * The logging buffers are byte queues that contain encoded printf-like
 * messages from GSP-RM.  They need to be decoded by a special application
 * that can parse the buffers.
 *
 * The 'loginit' buffer contains logs from early GSP-RM init and
 * exception dumps.  The 'logrm' buffer contains the subsequent logs. Both are
 * written to directly by GSP-RM and can be any multiple of GSP_PAGE_SIZE.
 *
 * The physical address map for the log buffer is stored in the buffer
 * itself, starting with offset 1. Offset 0 contains the "put" pointer (pp).
 * Initially, pp is equal to 0. If the buffer has valid logging data in it,
 * then pp points to index into the buffer where the next logging entry will
 * be written. Therefore, the logging data is valid if:
 *   1 <= pp < sizeof(buffer)/sizeof(u64)
 *
 * The GSP only understands 4K pages (GSP_PAGE_SIZE), so even if the kernel is
 * configured for a larger page size (e.g. 64K pages), we need to give
 * the GSP an array of 4K pages. Fortunately, since the buffer is
 * physically contiguous, it's simple math to calculate the addresses.
 *
 * The buffers must be a multiple of GSP_PAGE_SIZE.  GSP-RM also currently
 * ignores the @kind field for LOGINIT, LOGINTR, and LOGRM, but expects the
 * buffers to be physically contiguous anyway.
 *
 * The memory allocated for the arguments must remain until the GSP sends the
 * init_done RPC.
 *
 * See _kgspInitLibosLoggingStructures (allocates memory for buffers)
 * See kgspSetupLibosInitArgs_IMPL (creates pLibosInitArgs[] array)
 */
static int
r535_gsp_libos_init(struct nvkm_gsp *gsp)
{
	LibosMemoryRegionInitArgument *args;
	int ret;

	ret = nvkm_gsp_mem_ctor(gsp, 0x1000, &gsp->libos);
	if (ret)
		return ret;

	args = gsp->libos.data;

	ret = nvkm_gsp_mem_ctor(gsp, 0x10000, &gsp->loginit);
	if (ret)
		return ret;

	args[0].id8  = r535_gsp_libos_id8("LOGINIT");
	args[0].pa   = gsp->loginit.addr;
	args[0].size = gsp->loginit.size;
	args[0].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
	args[0].loc  = LIBOS_MEMORY_REGION_LOC_SYSMEM;
	create_pte_array(gsp->loginit.data + sizeof(u64), gsp->loginit.addr, gsp->loginit.size);

	ret = nvkm_gsp_mem_ctor(gsp, 0x10000, &gsp->logintr);
	if (ret)
		return ret;

	args[1].id8  = r535_gsp_libos_id8("LOGINTR");
	args[1].pa   = gsp->logintr.addr;
	args[1].size = gsp->logintr.size;
	args[1].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
	args[1].loc  = LIBOS_MEMORY_REGION_LOC_SYSMEM;
	create_pte_array(gsp->logintr.data + sizeof(u64), gsp->logintr.addr, gsp->logintr.size);

	ret = nvkm_gsp_mem_ctor(gsp, 0x10000, &gsp->logrm);
	if (ret)
		return ret;

	args[2].id8  = r535_gsp_libos_id8("LOGRM");
	args[2].pa   = gsp->logrm.addr;
	args[2].size = gsp->logrm.size;
	args[2].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
	args[2].loc  = LIBOS_MEMORY_REGION_LOC_SYSMEM;
	create_pte_array(gsp->logrm.data + sizeof(u64), gsp->logrm.addr, gsp->logrm.size);

	ret = r535_gsp_rmargs_init(gsp, false);
	if (ret)
		return ret;

	args[3].id8  = r535_gsp_libos_id8("RMARGS");
	args[3].pa   = gsp->rmargs.addr;
	args[3].size = gsp->rmargs.size;
	args[3].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
	args[3].loc  = LIBOS_MEMORY_REGION_LOC_SYSMEM;

#ifdef CONFIG_DEBUG_FS
	r535_gsp_libos_debugfs_init(gsp);
#endif

	return 0;
}

void
nvkm_gsp_sg_free(struct nvkm_device *device, struct sg_table *sgt)
{
	struct scatterlist *sgl;
	int i;

	dma_unmap_sgtable(device->dev, sgt, DMA_BIDIRECTIONAL, 0);

	for_each_sgtable_sg(sgt, sgl, i) {
		struct page *page = sg_page(sgl);

		__free_page(page);
	}

	sg_free_table(sgt);
}

int
nvkm_gsp_sg(struct nvkm_device *device, u64 size, struct sg_table *sgt)
{
	const u64 pages = DIV_ROUND_UP(size, PAGE_SIZE);
	struct scatterlist *sgl;
	int ret, i;

	ret = sg_alloc_table(sgt, pages, GFP_KERNEL);
	if (ret)
		return ret;

	for_each_sgtable_sg(sgt, sgl, i) {
		struct page *page = alloc_page(GFP_KERNEL);

		if (!page) {
			nvkm_gsp_sg_free(device, sgt);
			return -ENOMEM;
		}

		sg_set_page(sgl, page, PAGE_SIZE, 0);
	}

	ret = dma_map_sgtable(device->dev, sgt, DMA_BIDIRECTIONAL, 0);
	if (ret)
		nvkm_gsp_sg_free(device, sgt);

	return ret;
}

static void
nvkm_gsp_radix3_dtor(struct nvkm_gsp *gsp, struct nvkm_gsp_radix3 *rx3)
{
	nvkm_gsp_sg_free(gsp->subdev.device, &rx3->lvl2);
	nvkm_gsp_mem_dtor(&rx3->lvl1);
	nvkm_gsp_mem_dtor(&rx3->lvl0);
}

/**
 * nvkm_gsp_radix3_sg - build a radix3 table from a S/G list
 * @gsp: gsp pointer
 * @sgt: S/G list to traverse
 * @size: size of the image, in bytes
 * @rx3: radix3 array to update
 *
 * The GSP uses a three-level page table, called radix3, to map the firmware.
 * Each 64-bit "pointer" in the table is either the bus address of an entry in
 * the next table (for levels 0 and 1) or the bus address of the next page in
 * the GSP firmware image itself.
 *
 * Level 0 contains a single entry in one page that points to the first page
 * of level 1.
 *
 * Level 1, since it's also only one page in size, contains up to 512 entries,
 * one for each page in Level 2.
 *
 * Level 2 can be up to 512 pages in size, and each of those entries points to
 * the next page of the firmware image.  Since there can be up to 512*512
 * pages, that limits the size of the firmware to 512*512*GSP_PAGE_SIZE = 1GB.
 *
 * Internally, the GSP has its window into system memory, but the base
 * physical address of the aperture is not 0.  In fact, it varies depending on
 * the GPU architecture.  Since the GPU is a PCI device, this window is
 * accessed via DMA and is therefore bound by IOMMU translation.  The end
 * result is that GSP-RM must translate the bus addresses in the table to GSP
 * physical addresses.  All this should happen transparently.
 *
 * Returns 0 on success, or negative error code
 *
 * See kgspCreateRadix3_IMPL
 */
static int
nvkm_gsp_radix3_sg(struct nvkm_gsp *gsp, struct sg_table *sgt, u64 size,
		   struct nvkm_gsp_radix3 *rx3)
{
	struct sg_dma_page_iter sg_dma_iter;
	struct scatterlist *sg;
	size_t bufsize;
	u64 *pte;
	int ret, i, page_idx = 0;

	ret = nvkm_gsp_mem_ctor(gsp, GSP_PAGE_SIZE, &rx3->lvl0);
	if (ret)
		return ret;

	ret = nvkm_gsp_mem_ctor(gsp, GSP_PAGE_SIZE, &rx3->lvl1);
	if (ret)
		goto lvl1_fail;

	// Allocate level 2
	bufsize = ALIGN((size / GSP_PAGE_SIZE) * sizeof(u64), GSP_PAGE_SIZE);
	ret = nvkm_gsp_sg(gsp->subdev.device, bufsize, &rx3->lvl2);
	if (ret)
		goto lvl2_fail;

	// Write the bus address of level 1 to level 0
	pte = rx3->lvl0.data;
	*pte = rx3->lvl1.addr;

	// Write the bus address of each page in level 2 to level 1
	pte = rx3->lvl1.data;
	for_each_sgtable_dma_page(&rx3->lvl2, &sg_dma_iter, 0)
		*pte++ = sg_page_iter_dma_address(&sg_dma_iter);

	// Finally, write the bus address of each page in sgt to level 2
	for_each_sgtable_sg(&rx3->lvl2, sg, i) {
		void *sgl_end;

		pte = sg_virt(sg);
		sgl_end = (void *)pte + sg->length;

		for_each_sgtable_dma_page(sgt, &sg_dma_iter, page_idx) {
			*pte++ = sg_page_iter_dma_address(&sg_dma_iter);
			page_idx++;

			// Go to the next scatterlist for level 2 if we've reached the end
			if ((void *)pte >= sgl_end)
				break;
		}
	}

	if (ret) {
lvl2_fail:
		nvkm_gsp_mem_dtor(&rx3->lvl1);
lvl1_fail:
		nvkm_gsp_mem_dtor(&rx3->lvl0);
	}

	return ret;
}

int
r535_gsp_fini(struct nvkm_gsp *gsp, bool suspend)
{
	u32 mbox0 = 0xff, mbox1 = 0xff;
	int ret;

	if (!gsp->running)
		return 0;

	if (suspend) {
		GspFwWprMeta *meta = gsp->wpr_meta.data;
		u64 len = meta->gspFwWprEnd - meta->gspFwWprStart;
		GspFwSRMeta *sr;

		ret = nvkm_gsp_sg(gsp->subdev.device, len, &gsp->sr.sgt);
		if (ret)
			return ret;

		ret = nvkm_gsp_radix3_sg(gsp, &gsp->sr.sgt, len, &gsp->sr.radix3);
		if (ret)
			return ret;

		ret = nvkm_gsp_mem_ctor(gsp, sizeof(*sr), &gsp->sr.meta);
		if (ret)
			return ret;

		sr = gsp->sr.meta.data;
		sr->magic = GSP_FW_SR_META_MAGIC;
		sr->revision = GSP_FW_SR_META_REVISION;
		sr->sysmemAddrOfSuspendResumeData = gsp->sr.radix3.lvl0.addr;
		sr->sizeOfSuspendResumeData = len;

		mbox0 = lower_32_bits(gsp->sr.meta.addr);
		mbox1 = upper_32_bits(gsp->sr.meta.addr);
	}

	ret = r535_gsp_rpc_unloading_guest_driver(gsp, suspend);
	if (WARN_ON(ret))
		return ret;

	nvkm_msec(gsp->subdev.device, 2000,
		if (nvkm_falcon_rd32(&gsp->falcon, 0x040) == 0x80000000)
			break;
	);

	nvkm_falcon_reset(&gsp->falcon);

	ret = nvkm_gsp_fwsec_sb(gsp);
	WARN_ON(ret);

	ret = r535_gsp_booter_unload(gsp, mbox0, mbox1);
	WARN_ON(ret);

	gsp->running = false;
	return 0;
}

int
r535_gsp_init(struct nvkm_gsp *gsp)
{
	u32 mbox0, mbox1;
	int ret;

	if (!gsp->sr.meta.data) {
		mbox0 = lower_32_bits(gsp->wpr_meta.addr);
		mbox1 = upper_32_bits(gsp->wpr_meta.addr);
	} else {
		r535_gsp_rmargs_init(gsp, true);

		mbox0 = lower_32_bits(gsp->sr.meta.addr);
		mbox1 = upper_32_bits(gsp->sr.meta.addr);
	}

	/* Execute booter to handle (eventually...) booting GSP-RM. */
	ret = r535_gsp_booter_load(gsp, mbox0, mbox1);
	if (WARN_ON(ret))
		goto done;

	ret = r535_gsp_rpc_poll(gsp, NV_VGPU_MSG_EVENT_GSP_INIT_DONE);
	if (ret)
		goto done;

	gsp->running = true;

done:
	if (gsp->sr.meta.data) {
		nvkm_gsp_mem_dtor(&gsp->sr.meta);
		nvkm_gsp_radix3_dtor(gsp, &gsp->sr.radix3);
		nvkm_gsp_sg_free(gsp->subdev.device, &gsp->sr.sgt);
		return ret;
	}

	if (ret == 0)
		ret = r535_gsp_postinit(gsp);

	return ret;
}

static int
r535_gsp_rm_boot_ctor(struct nvkm_gsp *gsp)
{
	const struct firmware *fw = gsp->fws.bl;
	const struct nvfw_bin_hdr *hdr;
	RM_RISCV_UCODE_DESC *desc;
	int ret;

	hdr = nvfw_bin_hdr(&gsp->subdev, fw->data);
	desc = (void *)fw->data + hdr->header_offset;

	ret = nvkm_gsp_mem_ctor(gsp, hdr->data_size, &gsp->boot.fw);
	if (ret)
		return ret;

	memcpy(gsp->boot.fw.data, fw->data + hdr->data_offset, hdr->data_size);

	gsp->boot.code_offset = desc->monitorCodeOffset;
	gsp->boot.data_offset = desc->monitorDataOffset;
	gsp->boot.manifest_offset = desc->manifestOffset;
	gsp->boot.app_version = desc->appVersion;
	return 0;
}

static const struct nvkm_firmware_func
r535_gsp_fw = {
	.type = NVKM_FIRMWARE_IMG_SGT,
};

static int
r535_gsp_elf_section(struct nvkm_gsp *gsp, const char *name, const u8 **pdata, u64 *psize)
{
	const u8 *img = gsp->fws.rm->data;
	const struct elf64_hdr *ehdr = (const struct elf64_hdr *)img;
	const struct elf64_shdr *shdr = (const struct elf64_shdr *)&img[ehdr->e_shoff];
	const char *names = &img[shdr[ehdr->e_shstrndx].sh_offset];

	for (int i = 0; i < ehdr->e_shnum; i++, shdr++) {
		if (!strcmp(&names[shdr->sh_name], name)) {
			*pdata = &img[shdr->sh_offset];
			*psize = shdr->sh_size;
			return 0;
		}
	}

	nvkm_error(&gsp->subdev, "section '%s' not found\n", name);
	return -ENOENT;
}

static void
r535_gsp_dtor_fws(struct nvkm_gsp *gsp)
{
	nvkm_firmware_put(gsp->fws.bl);
	gsp->fws.bl = NULL;
	nvkm_firmware_put(gsp->fws.booter.unload);
	gsp->fws.booter.unload = NULL;
	nvkm_firmware_put(gsp->fws.booter.load);
	gsp->fws.booter.load = NULL;
	nvkm_firmware_put(gsp->fws.rm);
	gsp->fws.rm = NULL;
}

#ifdef CONFIG_DEBUG_FS

struct r535_gsp_log {
	struct nvif_log log;

	/*
	 * Logging buffers in debugfs. The wrapper objects need to remain
	 * in memory until the dentry is deleted.
	 */
	struct dentry *debugfs_logging_dir;
	struct debugfs_blob_wrapper blob_init;
	struct debugfs_blob_wrapper blob_intr;
	struct debugfs_blob_wrapper blob_rm;
	struct debugfs_blob_wrapper blob_pmu;
};

/**
 * r535_debugfs_shutdown - delete GSP-RM logging buffers for one GPU
 * @_log: nvif_log struct for this GPU
 *
 * Called when the driver is shutting down, to clean up the retained GSP-RM
 * logging buffers.
 */
static void r535_debugfs_shutdown(struct nvif_log *_log)
{
	struct r535_gsp_log *log = container_of(_log, struct r535_gsp_log, log);

	debugfs_remove(log->debugfs_logging_dir);

	kfree(log->blob_init.data);
	kfree(log->blob_intr.data);
	kfree(log->blob_rm.data);
	kfree(log->blob_pmu.data);

	/* We also need to delete the list object */
	kfree(log);
}

/**
 * is_empty - return true if the logging buffer was never written to
 * @b: blob wrapper with ->data field pointing to logging buffer
 *
 * The first 64-bit field of loginit, and logintr, and logrm is the 'put'
 * pointer, and it is initialized to 0. It's a dword-based index into the
 * circular buffer, indicating where the next printf write will be made.
 *
 * If the pointer is still 0 when GSP-RM is shut down, that means that the
 * buffer was never written to, so it can be ignored.
 *
 * This test also works for logpmu, even though it doesn't have a put pointer.
 */
static bool is_empty(const struct debugfs_blob_wrapper *b)
{
	u64 *put = b->data;

	return put ? (*put == 0) : true;
}

/**
 * r535_gsp_copy_log - preserve the logging buffers in a blob
 * @parent: the top-level dentry for this GPU
 * @name: name of debugfs entry to create
 * @s: original wrapper object to copy from
 * @t: new wrapper object to copy to
 *
 * When GSP shuts down, the nvkm_gsp object and all its memory is deleted.
 * To preserve the logging buffers, the buffers need to be copied, but only
 * if they actually have data.
 */
static int r535_gsp_copy_log(struct dentry *parent,
			     const char *name,
			     const struct debugfs_blob_wrapper *s,
			     struct debugfs_blob_wrapper *t)
{
	struct dentry *dent;
	void *p;

	if (is_empty(s))
		return 0;

	/* The original buffers will be deleted */
	p = kmemdup(s->data, s->size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	t->data = p;
	t->size = s->size;

	dent = debugfs_create_blob(name, 0444, parent, t);
	if (IS_ERR(dent)) {
		kfree(p);
		memset(t, 0, sizeof(*t));
		return PTR_ERR(dent);
	}

	i_size_write(d_inode(dent), t->size);

	return 0;
}

/**
 * r535_gsp_retain_logging - copy logging buffers to new debugfs root
 * @gsp: gsp pointer
 *
 * If keep_gsp_logging is enabled, then we want to preserve the GSP-RM logging
 * buffers and their debugfs entries, but all those objects would normally
 * deleted if GSP-RM fails to load.
 *
 * To preserve the logging buffers, we need to:
 *
 * 1) Allocate new buffers and copy the logs into them, so that the original
 * DMA buffers can be released.
 *
 * 2) Preserve the directories.  We don't need to save single dentries because
 * we're going to delete the parent when the
 *
 * If anything fails in this process, then all the dentries need to be
 * deleted.  We don't need to deallocate the original logging buffers because
 * the caller will do that regardless.
 */
static void r535_gsp_retain_logging(struct nvkm_gsp *gsp)
{
	struct device *dev = gsp->subdev.device->dev;
	struct r535_gsp_log *log = NULL;
	int ret;

	if (!keep_gsp_logging || !gsp->debugfs.parent) {
		/* Nothing to do */
		goto exit;
	}

	/* Check to make sure at least one buffer has data. */
	if (is_empty(&gsp->blob_init) && is_empty(&gsp->blob_intr) &&
	    is_empty(&gsp->blob_rm) && is_empty(&gsp->blob_rm)) {
		nvkm_warn(&gsp->subdev, "all logging buffers are empty\n");
		goto exit;
	}

	log = kzalloc(sizeof(*log), GFP_KERNEL);
	if (!log)
		goto error;

	/*
	 * Since the nvkm_gsp object is going away, the debugfs_blob_wrapper
	 * objects are also being deleted, which means the dentries will no
	 * longer be valid.  Delete the existing entries so that we can create
	 * new ones with the same name.
	 */
	debugfs_remove(gsp->debugfs.init);
	debugfs_remove(gsp->debugfs.intr);
	debugfs_remove(gsp->debugfs.rm);
	debugfs_remove(gsp->debugfs.pmu);

	ret = r535_gsp_copy_log(gsp->debugfs.parent, "loginit", &gsp->blob_init, &log->blob_init);
	if (ret)
		goto error;

	ret = r535_gsp_copy_log(gsp->debugfs.parent, "logintr", &gsp->blob_intr, &log->blob_intr);
	if (ret)
		goto error;

	ret = r535_gsp_copy_log(gsp->debugfs.parent, "logrm", &gsp->blob_rm, &log->blob_rm);
	if (ret)
		goto error;

	ret = r535_gsp_copy_log(gsp->debugfs.parent, "logpmu", &gsp->blob_pmu, &log->blob_pmu);
	if (ret)
		goto error;

	/* The nvkm_gsp object is going away, so save the dentry */
	log->debugfs_logging_dir = gsp->debugfs.parent;

	log->log.shutdown = r535_debugfs_shutdown;
	list_add(&log->log.entry, &gsp_logs.head);

	nvkm_warn(&gsp->subdev,
		  "logging buffers migrated to /sys/kernel/debug/nouveau/%s\n",
		  dev_name(dev));

	return;

error:
	nvkm_warn(&gsp->subdev, "failed to migrate logging buffers\n");

exit:
	debugfs_remove(gsp->debugfs.parent);

	if (log) {
		kfree(log->blob_init.data);
		kfree(log->blob_intr.data);
		kfree(log->blob_rm.data);
		kfree(log->blob_pmu.data);
		kfree(log);
	}
}

#endif

/**
 * r535_gsp_libos_debugfs_fini - cleanup/retain log buffers on shutdown
 * @gsp: gsp pointer
 *
 * If the log buffers are exposed via debugfs, the data for those entries
 * needs to be cleaned up when the GSP device shuts down.
 */
static void
r535_gsp_libos_debugfs_fini(struct nvkm_gsp __maybe_unused *gsp)
{
#ifdef CONFIG_DEBUG_FS
	r535_gsp_retain_logging(gsp);

	/*
	 * Unlike the other buffers, the PMU blob is a kmalloc'd buffer that
	 * exists only if the debugfs entries were created.
	 */
	kfree(gsp->blob_pmu.data);
	gsp->blob_pmu.data = NULL;
#endif
}

void
r535_gsp_dtor(struct nvkm_gsp *gsp)
{
	idr_destroy(&gsp->client_id.idr);
	mutex_destroy(&gsp->client_id.mutex);

	nvkm_gsp_radix3_dtor(gsp, &gsp->radix3);
	nvkm_gsp_mem_dtor(&gsp->sig);
	nvkm_firmware_dtor(&gsp->fw);

	nvkm_falcon_fw_dtor(&gsp->booter.unload);
	nvkm_falcon_fw_dtor(&gsp->booter.load);

	mutex_destroy(&gsp->msgq.mutex);
	mutex_destroy(&gsp->cmdq.mutex);

	r535_gsp_dtor_fws(gsp);

	nvkm_gsp_mem_dtor(&gsp->rmargs);
	nvkm_gsp_mem_dtor(&gsp->wpr_meta);
	nvkm_gsp_mem_dtor(&gsp->shm.mem);

	r535_gsp_libos_debugfs_fini(gsp);

	nvkm_gsp_mem_dtor(&gsp->loginit);
	nvkm_gsp_mem_dtor(&gsp->logintr);
	nvkm_gsp_mem_dtor(&gsp->logrm);
}

int
r535_gsp_oneinit(struct nvkm_gsp *gsp)
{
	struct nvkm_device *device = gsp->subdev.device;
	const u8 *data;
	u64 size;
	int ret;

	mutex_init(&gsp->cmdq.mutex);
	mutex_init(&gsp->msgq.mutex);

	ret = gsp->func->booter.ctor(gsp, "booter-load", gsp->fws.booter.load,
				     &device->sec2->falcon, &gsp->booter.load);
	if (ret)
		return ret;

	ret = gsp->func->booter.ctor(gsp, "booter-unload", gsp->fws.booter.unload,
				     &device->sec2->falcon, &gsp->booter.unload);
	if (ret)
		return ret;

	/* Load GSP firmware from ELF image into DMA-accessible memory. */
	ret = r535_gsp_elf_section(gsp, ".fwimage", &data, &size);
	if (ret)
		return ret;

	ret = nvkm_firmware_ctor(&r535_gsp_fw, "gsp-rm", device, data, size, &gsp->fw);
	if (ret)
		return ret;

	/* Load relevant signature from ELF image. */
	ret = r535_gsp_elf_section(gsp, gsp->func->sig_section, &data, &size);
	if (ret)
		return ret;

	ret = nvkm_gsp_mem_ctor(gsp, ALIGN(size, 256), &gsp->sig);
	if (ret)
		return ret;

	memcpy(gsp->sig.data, data, size);

	/* Build radix3 page table for ELF image. */
	ret = nvkm_gsp_radix3_sg(gsp, &gsp->fw.mem.sgt, gsp->fw.len, &gsp->radix3);
	if (ret)
		return ret;

	r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER,
			      r535_gsp_msg_run_cpu_sequencer, gsp);
	r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_POST_EVENT, r535_gsp_msg_post_event, gsp);
	r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_RC_TRIGGERED,
			      r535_gsp_msg_rc_triggered, gsp);
	r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_MMU_FAULT_QUEUED,
			      r535_gsp_msg_mmu_fault_queued, gsp);
	r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_OS_ERROR_LOG, r535_gsp_msg_os_error_log, gsp);
	r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_PERF_BRIDGELESS_INFO_UPDATE, NULL, NULL);
	r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_UCODE_LIBOS_PRINT, NULL, NULL);
	r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_GSP_SEND_USER_SHARED_DATA, NULL, NULL);
	ret = r535_gsp_rm_boot_ctor(gsp);
	if (ret)
		return ret;

	/* Release FW images - we've copied them to DMA buffers now. */
	r535_gsp_dtor_fws(gsp);

	/* Calculate FB layout. */
	gsp->fb.wpr2.frts.size = 0x100000;
	gsp->fb.wpr2.frts.addr = ALIGN_DOWN(gsp->fb.bios.addr, 0x20000) - gsp->fb.wpr2.frts.size;

	gsp->fb.wpr2.boot.size = gsp->boot.fw.size;
	gsp->fb.wpr2.boot.addr = ALIGN_DOWN(gsp->fb.wpr2.frts.addr - gsp->fb.wpr2.boot.size, 0x1000);

	gsp->fb.wpr2.elf.size = gsp->fw.len;
	gsp->fb.wpr2.elf.addr = ALIGN_DOWN(gsp->fb.wpr2.boot.addr - gsp->fb.wpr2.elf.size, 0x10000);

	{
		u32 fb_size_gb = DIV_ROUND_UP_ULL(gsp->fb.size, 1 << 30);

		gsp->fb.wpr2.heap.size =
			gsp->func->wpr_heap.os_carveout_size +
			gsp->func->wpr_heap.base_size +
			ALIGN(GSP_FW_HEAP_PARAM_SIZE_PER_GB_FB * fb_size_gb, 1 << 20) +
			ALIGN(GSP_FW_HEAP_PARAM_CLIENT_ALLOC_SIZE, 1 << 20);

		gsp->fb.wpr2.heap.size = max(gsp->fb.wpr2.heap.size, gsp->func->wpr_heap.min_size);
	}

	gsp->fb.wpr2.heap.addr = ALIGN_DOWN(gsp->fb.wpr2.elf.addr - gsp->fb.wpr2.heap.size, 0x100000);
	gsp->fb.wpr2.heap.size = ALIGN_DOWN(gsp->fb.wpr2.elf.addr - gsp->fb.wpr2.heap.addr, 0x100000);

	gsp->fb.wpr2.addr = ALIGN_DOWN(gsp->fb.wpr2.heap.addr - sizeof(GspFwWprMeta), 0x100000);
	gsp->fb.wpr2.size = gsp->fb.wpr2.frts.addr + gsp->fb.wpr2.frts.size - gsp->fb.wpr2.addr;

	gsp->fb.heap.size = 0x100000;
	gsp->fb.heap.addr = gsp->fb.wpr2.addr - gsp->fb.heap.size;

	ret = nvkm_gsp_fwsec_frts(gsp);
	if (WARN_ON(ret))
		return ret;

	ret = r535_gsp_libos_init(gsp);
	if (WARN_ON(ret))
		return ret;

	ret = r535_gsp_wpr_meta_init(gsp);
	if (WARN_ON(ret))
		return ret;

	ret = r535_gsp_rpc_set_system_info(gsp);
	if (WARN_ON(ret))
		return ret;

	ret = r535_gsp_rpc_set_registry(gsp);
	if (WARN_ON(ret))
		return ret;

	/* Reset GSP into RISC-V mode. */
	ret = gsp->func->reset(gsp);
	if (WARN_ON(ret))
		return ret;

	nvkm_falcon_wr32(&gsp->falcon, 0x040, lower_32_bits(gsp->libos.addr));
	nvkm_falcon_wr32(&gsp->falcon, 0x044, upper_32_bits(gsp->libos.addr));

	mutex_init(&gsp->client_id.mutex);
	idr_init(&gsp->client_id.idr);
	return 0;
}

static int
r535_gsp_load_fw(struct nvkm_gsp *gsp, const char *name, const char *ver,
		 const struct firmware **pfw)
{
	char fwname[64];

	snprintf(fwname, sizeof(fwname), "gsp/%s-%s", name, ver);
	return nvkm_firmware_get(&gsp->subdev, fwname, 0, pfw);
}

int
r535_gsp_load(struct nvkm_gsp *gsp, int ver, const struct nvkm_gsp_fwif *fwif)
{
	struct nvkm_subdev *subdev = &gsp->subdev;
	int ret;
	bool enable_gsp = fwif->enable;

#if IS_ENABLED(CONFIG_DRM_NOUVEAU_GSP_DEFAULT)
	enable_gsp = true;
#endif
	if (!nvkm_boolopt(subdev->device->cfgopt, "NvGspRm", enable_gsp))
		return -EINVAL;

	if ((ret = r535_gsp_load_fw(gsp, "gsp", fwif->ver, &gsp->fws.rm)) ||
	    (ret = r535_gsp_load_fw(gsp, "booter_load", fwif->ver, &gsp->fws.booter.load)) ||
	    (ret = r535_gsp_load_fw(gsp, "booter_unload", fwif->ver, &gsp->fws.booter.unload)) ||
	    (ret = r535_gsp_load_fw(gsp, "bootloader", fwif->ver, &gsp->fws.bl))) {
		r535_gsp_dtor_fws(gsp);
		return ret;
	}

	return 0;
}

#define NVKM_GSP_FIRMWARE(chip)                                  \
MODULE_FIRMWARE("nvidia/"#chip"/gsp/booter_load-535.113.01.bin");   \
MODULE_FIRMWARE("nvidia/"#chip"/gsp/booter_unload-535.113.01.bin"); \
MODULE_FIRMWARE("nvidia/"#chip"/gsp/bootloader-535.113.01.bin");    \
MODULE_FIRMWARE("nvidia/"#chip"/gsp/gsp-535.113.01.bin")

NVKM_GSP_FIRMWARE(tu102);
NVKM_GSP_FIRMWARE(tu104);
NVKM_GSP_FIRMWARE(tu106);

NVKM_GSP_FIRMWARE(tu116);
NVKM_GSP_FIRMWARE(tu117);

NVKM_GSP_FIRMWARE(ga100);

NVKM_GSP_FIRMWARE(ga102);
NVKM_GSP_FIRMWARE(ga103);
NVKM_GSP_FIRMWARE(ga104);
NVKM_GSP_FIRMWARE(ga106);
NVKM_GSP_FIRMWARE(ga107);

NVKM_GSP_FIRMWARE(ad102);
NVKM_GSP_FIRMWARE(ad103);
NVKM_GSP_FIRMWARE(ad104);
NVKM_GSP_FIRMWARE(ad106);
NVKM_GSP_FIRMWARE(ad107);

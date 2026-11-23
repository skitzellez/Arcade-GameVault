# one-off edit in deps/mesa-arcade (branch gv-arcade-dzn): DZN_DRED=1 turns on D3D12 DRED (breadcrumbs + page
# faults); when the device is lost, the reason, the last GPU operations and the faulting address go to Mesa's log
from pathlib import Path

root = Path(r"C:\Users\GamerDad\XboxEmu\deps\mesa-arcade\src\microsoft\vulkan")


def edit(name, pairs):
    p = root / name
    s = p.read_text(encoding="utf-8")
    for a, b in pairs:
        assert a in s, (name, a[:70])
        s = s.replace(a, b, 1)
    p.write_text(s, encoding="utf-8")


edit("dzn_util.c", [(
    '''void
d3d12_enable_gpu_validation(struct util_dl_library *d3d12_mod, ID3D12DeviceFactory *factory)
{''',
    '''/* Game Vault: DRED, D3D12's own record of what the GPU was doing when a device is lost (no debug layer needed) */
void
d3d12_enable_dred(struct util_dl_library *d3d12_mod, ID3D12DeviceFactory *factory)
{
   ID3D12DeviceRemovedExtendedDataSettings *dred = NULL;
   HRESULT hr;
   if (factory) {
      hr = ID3D12DeviceFactory_GetConfigurationInterface(factory, &CLSID_D3D12DeviceRemovedExtendedData,
                                                          &IID_ID3D12DeviceRemovedExtendedDataSettings, (void **)&dred);
   } else {
      typedef HRESULT(WINAPI *PFN_GET_DEBUG)(REFIID riid, void **out);
      PFN_GET_DEBUG get = (PFN_GET_DEBUG)util_dl_get_proc_address(d3d12_mod, "D3D12GetDebugInterface");
      hr = get ? get(&IID_ID3D12DeviceRemovedExtendedDataSettings, (void **)&dred) : E_FAIL;
   }
   if (FAILED(hr) || !dred) {
      mesa_loge("dzn: DRED not available (0x%08lx)", (unsigned long)hr);
      return;
   }
   ID3D12DeviceRemovedExtendedDataSettings_SetAutoBreadcrumbsEnablement(dred, D3D12_DRED_ENABLEMENT_FORCED_ON);
   ID3D12DeviceRemovedExtendedDataSettings_SetPageFaultEnablement(dred, D3D12_DRED_ENABLEMENT_FORCED_ON);
   ID3D12DeviceRemovedExtendedDataSettings_Release(dred);
   mesa_loge("dzn: DRED on");
}

void
d3d12_log_device_removed(ID3D12Device *dev)
{
   static bool logged = false;
   if (logged)
      return;
   logged = true;
   mesa_loge("dzn: device removed, reason 0x%08lx", (unsigned long)ID3D12Device_GetDeviceRemovedReason(dev));
   ID3D12DeviceRemovedExtendedData *dred = NULL;
   if (FAILED(ID3D12Device_QueryInterface(dev, &IID_ID3D12DeviceRemovedExtendedData, (void **)&dred)))
      return;
   D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc;
   if (SUCCEEDED(ID3D12DeviceRemovedExtendedData_GetAutoBreadcrumbsOutput(dred, &bc))) {
      int lists = 0;
      for (const D3D12_AUTO_BREADCRUMB_NODE *n = bc.pHeadAutoBreadcrumbNode; n && lists < 16; n = n->pNext, lists++) {
         UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
         if (done >= n->BreadcrumbCount)
            continue;   /* finished command lists */
         mesa_loge("dzn:   command list %p: %u of %u operations done", (void *)n->pCommandList, done, n->BreadcrumbCount);
         UINT from = done > 4 ? done - 4 : 0, to = MIN2(n->BreadcrumbCount, done + 4);
         for (UINT i = from; i < to; i++)
            mesa_loge("dzn:     %s op %u: %d", i == done ? ">>" : "  ", i, (int)n->pCommandHistory[i]);
      }
   }
   D3D12_DRED_PAGE_FAULT_OUTPUT pf;
   if (SUCCEEDED(ID3D12DeviceRemovedExtendedData_GetPageFaultAllocationOutput(dred, &pf)) && pf.PageFaultVA)
      mesa_loge("dzn:   page fault at GPU address 0x%llx", (unsigned long long)pf.PageFaultVA);
   ID3D12DeviceRemovedExtendedData_Release(dred);
}

void
d3d12_enable_gpu_validation(struct util_dl_library *d3d12_mod, ID3D12DeviceFactory *factory)
{'''),
])
edit("dzn_private.h", [(
    'd3d12_enable_gpu_validation(struct util_dl_library *d3d12_mod, ID3D12DeviceFactory *factory);',
    '''d3d12_enable_gpu_validation(struct util_dl_library *d3d12_mod, ID3D12DeviceFactory *factory);

void
d3d12_enable_dred(struct util_dl_library *d3d12_mod, ID3D12DeviceFactory *factory);

void
d3d12_log_device_removed(ID3D12Device *dev);'''),
])
edit("dzn_device.c", [
    ('''   if (instance->debug_flags & DZN_DEBUG_GBV)
      d3d12_enable_gpu_validation(instance->d3d12_mod, instance->factory);''',
     '''   if (instance->debug_flags & DZN_DEBUG_GBV)
      d3d12_enable_gpu_validation(instance->d3d12_mod, instance->factory);
   if (os_get_option("DZN_DRED"))   /* Game Vault: what the GPU did when a device is lost */
      d3d12_enable_dred(instance->d3d12_mod, instance->factory);'''),
    ('''   if (FAILED(ID3D12Device_GetDeviceRemovedReason(device->dev)))
      return vk_device_set_lost(&device->vk, "D3D12 device removed");''',
     '''   if (FAILED(ID3D12Device_GetDeviceRemovedReason(device->dev))) {
      d3d12_log_device_removed((ID3D12Device *)device->dev);
      return vk_device_set_lost(&device->vk, "D3D12 device removed");
   }'''),
])
print("ok")

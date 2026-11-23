# one-off edit in deps/mesa-arcade (branch gv-arcade-dzn): on Xbox, one DXGI swap chain per CoreWindow surface,
# kept for the surface's life and resized for each new Vulkan swap chain
from pathlib import Path

p = Path(r"C:\Users\GamerDad\XboxEmu\deps\mesa-arcade\src\vulkan\wsi\wsi_common_win32.cpp")
s = p.read_text(encoding="utf-8")


def rep(a, b):
    global s
    assert a in s, a[:80]
    s = s.replace(a, b, 1)


rep('''#if !defined _XBOX_UWP
   IDCompositionTarget *target;
   IDCompositionVisual *visual;
#endif
''', '''#if !defined _XBOX_UWP
   IDCompositionTarget *target;
   IDCompositionVisual *visual;
#else
   /* Game Vault: a CoreWindow keeps one DXGI swap chain for its life (a second one for the same window is refused
    * while the first exists); each new Vulkan swap chain resizes it */
   IDXGISwapChain3 *uwp_dxgi;
#endif
''')
rep('''   VK_FROM_HANDLE(vk_instance, instance, _instance);
#if !defined _XBOX_UWP
   wsi_win32_surface *surface = (wsi_win32_surface *)icd_surface;
   if (surface->visual)
      surface->visual->Release();
   if (surface->target)
      surface->target->Release();
#endif''', '''   VK_FROM_HANDLE(vk_instance, instance, _instance);
   wsi_win32_surface *surface = (wsi_win32_surface *)icd_surface;
#if !defined _XBOX_UWP
   if (surface->visual)
      surface->visual->Release();
   if (surface->target)
      surface->target->Release();
#else
   if (surface->uwp_dxgi)
      surface->uwp_dxgi->Release();
#endif''')

start = s.index('''#else
   HRESULT hr = factory->CreateSwapChainForCoreWindow(''')
end = s.index('''   swapchain1->Release();
#if !defined _XBOX_UWP
   if (!surface->target &&''')
new_block = '''   swapchain1->Release();
#else
   if (surface->uwp_dxgi) {
      /* the window's swap chain again, at the new size (the old Vulkan swap chain has given its images back) */
      HRESULT hr = surface->uwp_dxgi->ResizeBuffers(desc.BufferCount, desc.Width, desc.Height, desc.Format, desc.Flags);
      if (FAILED(hr)) {
         mesa_loge("wsi: ResizeBuffers %ux%u, %u buffers, flags 0x%x: 0x%08lx", desc.Width, desc.Height,
                   desc.BufferCount, desc.Flags, (unsigned long)hr);
         return VK_ERROR_INITIALIZATION_FAILED;
      }
   } else {
      HRESULT hr = factory->CreateSwapChainForCoreWindow(queue, static_cast<IUnknown*>(uwp_get_window_reference()),
                                                         &desc, nullptr, &swapchain1);
      if (FAILED(hr)) {
         mesa_loge("wsi: CreateSwapChainForCoreWindow %ux%u, %u buffers, usage 0x%x, flags 0x%x: 0x%08lx",
                   desc.Width, desc.Height, desc.BufferCount, (unsigned)desc.BufferUsage, desc.Flags, (unsigned long)hr);
         return VK_ERROR_INITIALIZATION_FAILED;
      }
      hr = swapchain1->QueryInterface(&surface->uwp_dxgi);
      swapchain1->Release();
      if (FAILED(hr))
         return VK_ERROR_INITIALIZATION_FAILED;
   }
   surface->uwp_dxgi->AddRef();
   chain->dxgi = surface->uwp_dxgi;
#endif
'''
s = s[:start] + new_block + s[end:].replace('''   swapchain1->Release();
#if !defined _XBOX_UWP
   if (!surface->target &&''', '''#if !defined _XBOX_UWP
   if (!surface->target &&''', 1)
p.write_text(s, encoding="utf-8")
print("ok")

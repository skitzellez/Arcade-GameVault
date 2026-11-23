# one-off edit in deps/mesa-arcade (gv-arcade-dzn): on Xbox, when the host exports MesaUwpHostPresent, swap chains
# use CPU images and every presented frame goes to the host (the arcade loader shows it through a D3D11 swap chain:
# a D3D12 CoreWindow swap chain made by Dozen stays black on the Xbox)
from pathlib import Path

p = Path(r"C:\Users\GamerDad\XboxEmu\deps\mesa-arcade\src\vulkan\wsi\wsi_common_win32.cpp")
s = p.read_text(encoding="utf-8")


def rep(a, b):
    global s
    assert a in s, a[:70]
    s = s.replace(a, b, 1)


rep('''static const struct {
   VkFormat     format;
} available_surface_formats[] = {''', r'''/* Game Vault: the host's present function (MesaUwpHostPresent, exported by the arcade loader's exe): pixels of a
 * presented frame, rows of 'pitch' bytes, VkFormat 'format' */
typedef void (*wsi_host_present_t)(const void *pixels, int pitch, int width, int height, int format);
static wsi_host_present_t
wsi_host_present(void)
{
#ifdef _XBOX_UWP
   static int looked = 0;
   static wsi_host_present_t fn = NULL;
   if (!looked) {
      fn = (wsi_host_present_t)GetProcAddress(GetModuleHandleW(NULL), "MesaUwpHostPresent");
      looked = 1;
   }
   return fn;
#else
   return NULL;
#endif
}

static const struct {
   VkFormat     format;
} available_surface_formats[] = {''')
rep('''   for (unsigned i = 0; i < ARRAY_SIZE(available_surface_formats); i++)
      sorted_formats[i] = available_surface_formats[i].format;
''', '''   for (unsigned i = 0; i < ARRAY_SIZE(available_surface_formats); i++)
      sorted_formats[i] = available_surface_formats[i].format;
   if (wsi_host_present()) {   /* the host shows RGBA: that one first */
      sorted_formats[0] = VK_FORMAT_R8G8B8A8_UNORM;
      sorted_formats[1] = VK_FORMAT_B8G8R8A8_UNORM;
   }
''')
rep('''   bool supports_dxgi = wsi->dxgi.factory &&
#ifndef _XBOX_UWP''', '''   bool supports_dxgi = !wsi_host_present() && wsi->dxgi.factory &&
#ifndef _XBOX_UWP''')
rep('''   if (chain->dxgi)
      return VK_SUCCESS;

#ifndef _XBOX_UWP
   chain->chain_dc = GetDC(chain->wnd);''', '''   if (chain->dxgi)
      return VK_SUCCESS;
   if (wsi_host_present())   /* CPU image: presented to the host */
      return VK_SUCCESS;

#ifndef _XBOX_UWP
   chain->chain_dc = GetDC(chain->wnd);''')
rep('''   if (chain->dxgi)
      return wsi_win32_queue_present_dxgi(chain, image, damage);
''', '''   if (chain->dxgi)
      return wsi_win32_queue_present_dxgi(chain, image, damage);
   if (wsi_host_present_t host = wsi_host_present()) {
      host(image->base.cpu_map, (int)image->base.row_pitches[0], (int)chain->extent.width,
           (int)chain->extent.height, (int)chain->base.image_info.create.format);
      wsi_win32_set_image_idle(chain, image);
      return chain->status;
   }
''')
p.write_text(s, encoding="utf-8")
print("ok")

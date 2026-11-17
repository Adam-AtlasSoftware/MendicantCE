/*
HOST_DISPLAY_WIN.C

display.hdr (#1122): what Windows says of the display a window is on that
SDL does not (its headroom and SDR white are SDL's: host_vulkan.c), its
black and its full-frame peak in nits, as DXGI's IDXGIOutput6::GetDesc1
reports them (the Windows HDR Calibration app's values where it has run).
The output is the one whose monitor the window is on, found on the adapter
whose LUID is the Vulkan device's, else on any adapter (a laptop's display
on its integrated GPU while another draws). dxgi.dll is loaded as it is
asked, so that nothing links it.
*/

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_6.h>
#include <string.h>

/* (their own, so that dxguid.lib is not needed) */
static const GUID factory_iid = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
static const GUID output6_iid = { 0x068346e8, 0xaaec, 0x4b84, { 0xad, 0xd7, 0x13, 0x7f, 0x51, 0x3f, 0x77, 0xa1 } };

/* the output of an adapter that shows a monitor, its description read
into description; 0 if none does */
static int adapter_output(IDXGIAdapter1 *adapter, HMONITOR monitor, DXGI_OUTPUT_DESC1 *description)
{
	IDXGIOutput *output;
	UINT index;

	for (index = 0; IDXGIAdapter1_EnumOutputs(adapter, index, &output) == S_OK; index++)
	{
		DXGI_OUTPUT_DESC plain;
		IDXGIOutput6 *output6 = NULL;
		int found = 0;

		if (IDXGIOutput_GetDesc(output, &plain) == S_OK && plain.Monitor == monitor &&
			IDXGIOutput_QueryInterface(output, &output6_iid, (void **)&output6) == S_OK)
		{
			found = IDXGIOutput6_GetDesc1(output6, description) == S_OK;
			IDXGIOutput6_Release(output6);
		}
		IDXGIOutput_Release(output);
		if (found)
			return 1;
	}
	return 0;
}

int host_display_luminance(const unsigned char luid[8], void *window, float *black_nits, float *peak_nits,
	float *full_frame_nits, float primaries[8])
{
	typedef HRESULT (WINAPI *create_factory_function)(REFIID, void **);
	static HMODULE dxgi;
	static create_factory_function create_factory;
	DXGI_OUTPUT_DESC1 description;
	IDXGIFactory1 *factory = NULL;
	IDXGIAdapter1 *adapter;
	HMONITOR monitor;
	UINT index;
	int pass, found = 0;

	*black_nits = *peak_nits = *full_frame_nits = 0.0f;
	if (!window)
		return 0;
	monitor = MonitorFromWindow((HWND)window, MONITOR_DEFAULTTONEAREST);
	if (!monitor)
		return 0;
	if (!dxgi)
	{
		dxgi = LoadLibraryW(L"dxgi.dll");
		if (dxgi)
			create_factory = (create_factory_function)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1");
	}
	/* (a factory of its own each time: one's outputs keep the descriptions
	they had when it was made) */
	if (!create_factory || create_factory(&factory_iid, (void **)&factory) != S_OK || !factory)
		return 0;
	/* (the Vulkan device's adapter first; then any) */
	for (pass = luid ? 0 : 1; pass < 2 && !found; pass++)
	{
		for (index = 0; !found && IDXGIFactory1_EnumAdapters1(factory, index, &adapter) == S_OK; index++)
		{
			DXGI_ADAPTER_DESC1 adapter_description;
			int ours = luid && IDXGIAdapter1_GetDesc1(adapter, &adapter_description) == S_OK &&
				!memcmp(&adapter_description.AdapterLuid, luid, 8);

			if (pass == 1 || ours)
				found = adapter_output(adapter, monitor, &description);
			IDXGIAdapter1_Release(adapter);
		}
	}
	IDXGIFactory1_Release(factory);
	if (!found)
		return 0;
	*black_nits = description.MinLuminance > 0.0f ? description.MinLuminance : 0.0f;
	*peak_nits = description.MaxLuminance > 0.0f ? description.MaxLuminance : 0.0f;
	*full_frame_nits = description.MaxFullFrameLuminance > 0.0f ? description.MaxFullFrameLuminance : 0.0f;
	if (primaries)
	{
		primaries[0] = description.RedPrimary[0];
		primaries[1] = description.RedPrimary[1];
		primaries[2] = description.GreenPrimary[0];
		primaries[3] = description.GreenPrimary[1];
		primaries[4] = description.BluePrimary[0];
		primaries[5] = description.BluePrimary[1];
		primaries[6] = description.WhitePoint[0];
		primaries[7] = description.WhitePoint[1];
	}
	return 1;
}

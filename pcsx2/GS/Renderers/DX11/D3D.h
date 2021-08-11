#pragma once

#include <dxgi1_3.h>
#include <vector>
#include <string>
#include <wil/com.h>

namespace D3D
{
	// create a dxgi factory
	wil::com_ptr_nothrow<IDXGIFactory2> CreateFactory(bool debug);

	// get an adapter based on position
	// assuming no one removes/moves it, it should always have the same id
	// however in the event that the adapter is not found due to the above, use the default
	wil::com_ptr_nothrow<IDXGIAdapter1> GetAdapterFromIndex(IDXGIFactory2* factory, int index);

	// this is sort of a legacy thing that doesn't have much to do with d3d (just the easiest way)
	// checks to see if the adapter at 0 is NV and thus we should prefer OpenGL
	bool IsNvidia(IDXGIAdapter1* adapter);
	bool ShouldPreferD3D();
};

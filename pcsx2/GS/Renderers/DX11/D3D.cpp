#include "PrecompiledHeader.h"
#include "GS/Renderers/DX11/D3D.h"
#include "GS/GS.h"

#include <d3d11.h>

namespace D3D
{
	wil::com_ptr_nothrow<IDXGIFactory2> CreateFactory(bool debug)
	{
		UINT flags = 0;
		if (debug)
			flags |= DXGI_CREATE_FACTORY_DEBUG;

		// we use CreateDXGIFactory2 because we assume at least windows 8.1 anyway
		wil::com_ptr_nothrow<IDXGIFactory2> factory;
		HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(factory.put()));

		// if we failed to create a factory with debug support
		// try one without
		if (FAILED(hr) && debug)
		{
			fprintf(stderr, "D3D: failed to create debug dxgi factory, trying without debugging\n");
			hr = CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put()));;
		}

		if (FAILED(hr))
		{
			fprintf(stderr, "D3D: failed to create dxgi factory\n"
				"check that your system meets our minimum requirements:\n"
				"https://github.com/PCSX2/pcsx2#system-requirements\n");
			return nullptr;
		}

		return factory;
	}

	wil::com_ptr_nothrow<IDXGIAdapter1> GetAdapterFromIndex(IDXGIFactory2* factory, int index)
	{
		ASSERT(factory);

		wil::com_ptr_nothrow<IDXGIAdapter1> adapter;
		if (factory->EnumAdapters1(index, adapter.put()) == DXGI_ERROR_NOT_FOUND)
		{
			// try index 0 (default adapter)
			fprintf(stderr, "D3D: adapter not found, falling back to the default\n");
			if (FAILED(factory->EnumAdapters1(0, adapter.put())))
			{
				// either there are no adapters connected or something major is wrong with the system
				fprintf(stderr, "D3D: failed to EnumAdapters\n");
				return nullptr;
			}
		}

		return adapter;
	}

	bool IsNvidia(IDXGIAdapter1* adapter)
	{
		ASSERT(adapter);

		DXGI_ADAPTER_DESC1 desc = {};
		if (FAILED(adapter->GetDesc1(&desc)))
		{
			fprintf(stderr, "D3D: failed to get the adapter description\n");
			return false;
		}

		// NV magic number
		if (desc.VendorId != 0x10DE)
			return false;

		return true;
	}

	bool SupportsFeatureLevel11(IDXGIAdapter1* adapter)
	{
		ASSERT(adapter);

		D3D_FEATURE_LEVEL feature_level;

		// don't need a context and d3d will default to checking for these feature levels:
		// D3D_FEATURE_LEVEL_11_0
		// D3D_FEATURE_LEVEL_10_1
		// D3D_FEATURE_LEVEL_10_0
		// D3D_FEATURE_LEVEL_9_3
		// D3D_FEATURE_LEVEL_9_2
		// D3D_FEATURE_LEVEL_9_1
		wil::com_ptr_nothrow<ID3D11Device> device;
		const HRESULT hr = D3D11CreateDevice(
			adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
			NULL, 0, D3D11_SDK_VERSION, &device, &feature_level, nullptr
		);

		if (FAILED(hr))
			return false;

		return feature_level == D3D_FEATURE_LEVEL_11_0;
	}

	bool ShouldPreferD3D()
	{
		auto factory = CreateFactory(false);
		auto adapter = GetAdapterFromIndex(factory.get(), 0);

		return !(IsNvidia(adapter.get()) && SupportsFeatureLevel11(adapter.get()));
	}
}

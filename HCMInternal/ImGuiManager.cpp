#include "pch.h"
#include "ImGuiManager.h"
#include "GlobalKill.h"
#include "ProggyVectorRegularFont.h"
#include "ModMenuFonts.h"
#include "imgui_internal.h"





ImGuiManager* ImGuiManager::instance = nullptr;

WNDPROC ImGuiManager::mOldWndProc = nullptr;
IMGUI_IMPL_API LRESULT  ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT __stdcall ImGuiManager::mNewWndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	//https://www.unknowncheats.me/forum/2488829-post5.html
	ImGuiIO& io = ImGui::GetIO();
	LRESULT res = ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);


	switch (uMsg)
	{
	//case WM_CLOSE:
	case WM_DESTROY:
	case WM_NCDESTROY:
		GlobalKill::killMe();
		break;
	default:
		break;
	}

	if (io.WantCaptureMouse == false)
	{
		// ImGui didn't handle the click so let MCC do it
		return CallWindowProc(ImGuiManager::mOldWndProc, hWnd, uMsg, wParam, lParam);
	}
	else
	{
		switch (uMsg)
		{
			// Certain messages we want to let MCC know about too
		case WM_ACTIVATE:
		case WM_ACTIVATEAPP:
		case WM_NCACTIVATE:
			return CallWindowProc(ImGuiManager::mOldWndProc, hWnd, uMsg, wParam, lParam);
			break;
		default:
			return true; // otherwise we just tell MCC to not worry about it
		}
	}





}

void ImGuiManager::initializeImGuiResources(ID3D11Device* pDevice, ID3D11DeviceContext* pDeviceContext, IDXGISwapChain* pSwapChain, ID3D11RenderTargetView* pMainRenderTargetView)
{
	PLOG_VERBOSE << "initializeImGuiResources";
	// Use swap chain description to get MCC window handle
	DXGI_SWAP_CHAIN_DESC swapChainDesc;
	pSwapChain->GetDesc(&swapChainDesc);
	if (!&swapChainDesc) throw HCMInitException("Failed to get swap chain description");
	m_windowHandle = swapChainDesc.OutputWindow;

	// Setup the imgui WndProc callback
	mOldWndProc = (WNDPROC)SetWindowLongPtrW(m_windowHandle, GWLP_WNDPROC, (LONG_PTR)&mNewWndProc);


	// Setup ImGui stuff
	PLOG_DEBUG << "Initializing ImGui";
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags = ImGuiConfigFlags_NoMouseCursorChange;// | ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

	if (!ImGui_ImplWin32_Init(m_windowHandle))
	{
		throw HCMInitException(std::format("ImGui_ImplWin32_Init failed w/ {} ", (uint64_t)m_windowHandle).c_str());
	};
	if (!ImGui_ImplDX11_Init(pDevice, pDeviceContext))
	{
		throw HCMInitException(std::format("ImGui_ImplDX11_Init failed w/ {}, {} ", (uint64_t)pDevice, (uint64_t)pDeviceContext).c_str());
	};

	PLOG_DEBUG << "ImGui Initialized";

	// Setup Dear ImGui style: black lacquer and champagne gold, to match the Mod Menu.
	// Sizes that HCM's hand-tuned layout depends on (13px text, 3px frame padding, 4px item spacing) are left alone.
	ImGui::StyleColorsDark();
	ImGuiStyle* style = &ImGui::GetStyle();
	style->WindowRounding = 10.f;
	style->ChildRounding = 6.f;
	style->FrameRounding = 4.f;
	style->PopupRounding = 8.f;
	style->GrabRounding = 3.f;
	style->ScrollbarRounding = 4.f;
	style->TabRounding = 4.f;
	style->WindowBorderSize = 1.f;
	style->FrameBorderSize = 0.f;
	style->PopupBorderSize = 1.f;
	style->ScrollbarSize = 10.f;
	style->WindowTitleAlign = ImVec2(0.02f, 0.5f);
	style->IndentSpacing = 10.f;

	auto rgba = [](int r, int g, int b, float a = 1.f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); };
	const ImVec4 gold = rgba(212, 175, 106), goldBright = rgba(243, 220, 170), ivory = rgba(238, 231, 216);
	auto withA = [](ImVec4 c, float a) { c.w = a; return c; };
	ImVec4* c = style->Colors;
	c[ImGuiCol_Text] = ivory;
	c[ImGuiCol_TextDisabled] = rgba(114, 106, 94);
	c[ImGuiCol_WindowBg] = rgba(11, 10, 8, 0.95f);
	c[ImGuiCol_ChildBg] = rgba(255, 244, 222, 0.025f);
	c[ImGuiCol_PopupBg] = rgba(19, 17, 14, 0.98f);
	c[ImGuiCol_Border] = withA(gold, 0.22f);
	c[ImGuiCol_BorderShadow] = rgba(0, 0, 0, 0.f);
	c[ImGuiCol_FrameBg] = rgba(255, 244, 222, 0.06f);
	c[ImGuiCol_FrameBgHovered] = withA(gold, 0.16f);
	c[ImGuiCol_FrameBgActive] = withA(gold, 0.26f);
	c[ImGuiCol_TitleBg] = rgba(11, 10, 8, 0.95f);
	c[ImGuiCol_TitleBgActive] = rgba(24, 21, 16, 0.97f);
	c[ImGuiCol_TitleBgCollapsed] = rgba(11, 10, 8, 0.80f);
	c[ImGuiCol_MenuBarBg] = rgba(19, 17, 14, 1.f);
	c[ImGuiCol_ScrollbarBg] = rgba(0, 0, 0, 0.f);
	c[ImGuiCol_ScrollbarGrab] = withA(gold, 0.22f);
	c[ImGuiCol_ScrollbarGrabHovered] = withA(gold, 0.40f);
	c[ImGuiCol_ScrollbarGrabActive] = withA(gold, 0.55f);
	c[ImGuiCol_CheckMark] = gold;
	c[ImGuiCol_SliderGrab] = gold;
	c[ImGuiCol_SliderGrabActive] = goldBright;
	c[ImGuiCol_Button] = withA(gold, 0.12f);
	c[ImGuiCol_ButtonHovered] = withA(gold, 0.28f);
	c[ImGuiCol_ButtonActive] = withA(gold, 0.42f);
	c[ImGuiCol_Header] = withA(gold, 0.14f);
	c[ImGuiCol_HeaderHovered] = withA(gold, 0.24f);
	c[ImGuiCol_HeaderActive] = withA(gold, 0.34f);
	c[ImGuiCol_Separator] = withA(gold, 0.20f);
	c[ImGuiCol_SeparatorHovered] = withA(gold, 0.45f);
	c[ImGuiCol_SeparatorActive] = gold;
	c[ImGuiCol_ResizeGrip] = withA(gold, 0.15f);
	c[ImGuiCol_ResizeGripHovered] = withA(gold, 0.35f);
	c[ImGuiCol_ResizeGripActive] = withA(gold, 0.55f);
	c[ImGuiCol_Tab] = withA(gold, 0.10f);
	c[ImGuiCol_TabHovered] = withA(gold, 0.28f);
	c[ImGuiCol_PlotLines] = gold;
	c[ImGuiCol_PlotHistogram] = gold;
	c[ImGuiCol_TextSelectedBg] = withA(gold, 0.30f);
	c[ImGuiCol_ModalWindowDimBg] = rgba(0, 0, 0, 0.45f);

	// setup font
	// Manrope at 13px goes first so it becomes HCM's default UI font (same height as ImGui's old 13px default,
	// so HCM's fixed row heights still fit). ProggyClean stays loaded for anything that asks for it.
	ModMenuFonts::addDefaultUIFont(io.Fonts, 13.f);
	io.Fonts->AddFontDefault();
	mRescalableMonospacedFont = io.Fonts->AddFontFromMemoryCompressedTTF(ProggyVectorRegularFont_compressed_data, ProggyVectorRegularFont_compressed_size, 15.f * 2);
	ModMenuFonts::addMenuFonts(io.Fonts); // Cinzel + Manrope at large sizes for the Mod Menu

	//int my_image_width = 0;
	//int my_image_height = 0;

	//bool ret = LoadTextureFromMemory(103, pDevice, &my_texture, &my_image_width, &my_image_height);
	//// todo; overlay bypass stuff based on return value
	//IM_ASSERT(ret);


}


ImGuiManager::~ImGuiManager()
{
	PLOG_DEBUG << "~ImGuiManager()";
	//presentEventCallback.~ScopedCallback();
	presentEventCallback.removeCallback(); // no new callback invokes
	if (mOldWndProc) 		// restore the original wndProc
	{
		SetWindowLongPtrW(m_windowHandle, GWLP_WNDPROC, (LONG_PTR)mOldWndProc);
		mOldWndProc = nullptr;
	}

	std::unique_lock<std::mutex> lock(mDestructionGuard); // block until callbacks finish executing



	// Cleanup
	if (m_isImguiInitialized)
	{
		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();

		m_isImguiInitialized = false;
	}

	instance = nullptr;
}



//void ImGuiManager::lapuaTest(SimpleMath::Vector2 ss)
//{
//	constexpr auto strength = 0x01FFFFFF;
//	constexpr auto zero = ImVec2(0, 0);
//	constexpr auto one = ImVec2(1, 1);
//	ImGui::GetBackgroundDrawList()->AddImage(my_texture, zero, ss, zero, one, strength);
//
//}

void ImGuiManager::onPresentHookEvent(ID3D11Device* pDevice, ID3D11DeviceContext* pDeviceContext, IDXGISwapChain* pSwapChain, ID3D11RenderTargetView* pMainRenderTargetView)
{
	LOG_ONCE(PLOG_DEBUG << "ImGuiManager::onPresentHookEvent running");
	std::unique_lock<std::mutex> lock(mDestructionGuard);
#pragma region init


	if (!instance->m_isImguiInitialized)
	{
		PLOG_INFO << "Initializing ImGuiManager";
		try
		{
			instance->initializeImGuiResources(pDevice, pDeviceContext, pSwapChain, pMainRenderTargetView);
			instance->m_isImguiInitialized = true;
		}
		catch (HCMInitException& ex)
		{
			PLOG_FATAL << "Failed to initialize ImGui, info: " << std::endl
				<< ex.what() << std::endl
				<< "HCM will now automatically close down";
			GlobalKill::killMe();
			return;
		}
	}
#pragma endregion init

	//TODO: check if this is necessary
	MSG msg;
	while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
	{
		::TranslateMessage(&msg);
		::DispatchMessage(&msg);

	}


	// Start ImGui frame
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	GImGui->NavWindowingTarget = nullptr; // prevent navinputs altogether

	// I don't 100% understand what this does, but it must be done before we try to render
	pDeviceContext->OMSetRenderTargets(1, &pMainRenderTargetView, NULL);
	auto screenSize = getScreenSize();


	// invoke callback of anything that wants to render with ImGui
	BackgroundRenderEvent->operator()(screenSize); // for overlays.
	MidgroundRenderEvent->operator()(screenSize);
	ForegroundRenderEvent->operator()(screenSize);
	ForegroundDirectXRenderEvent->operator()(pDevice, pDeviceContext, screenSize, pMainRenderTargetView);

	// Finish ImGui frame
	ImGui::EndFrame();
	ImGui::Render();

	// Render it !
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}



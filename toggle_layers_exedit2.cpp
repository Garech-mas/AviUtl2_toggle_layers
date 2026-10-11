#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <set>
#include <vector>

#include "aviutl2_sdk/plugin2.h"
#include "aviutl2_sdk/logger2.h"

#include <cstdarg>

static LOG_HANDLE* g_logger = nullptr;

static void LogVerbose(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, fmt, ap);
    va_end(ap);
    if (g_logger != nullptr && g_logger->verbose != nullptr) {
        g_logger->verbose(g_logger, buf);
    }
    else {
        OutputDebugStringW(buf);
    }
}

namespace {

    constexpr DWORD kRequiredVersion = 2004500; // 必須バージョン: beta45

    HINSTANCE g_instance = nullptr;
    EDIT_HANDLE* g_edit_handle = nullptr;
    HOST_APP_TABLE* g_host = nullptr;
    HWND g_message_window = nullptr;
    HHOOK g_mouse_hook = nullptr;
    bool g_input_capture = false;

    constexpr wchar_t kMessageWindowClass[] = L"ToggleLayersExEdit2MessageWindow";
    constexpr UINT kMessageDragStart = WM_APP + 1;
    constexpr UINT kMessageDragMove = WM_APP + 2;
    constexpr UINT kMessageDragFinish = WM_APP + 3;

    struct DragState {
        bool active = false;
        int probe_x = 0;
        int last_layer = 0;
        bool enable = true;
        bool lock_mode = false;
    };

    DragState g_drag;

    COMMON_PLUGIN_TABLE g_plugin_table = {
        L"レイヤー一括切り替え",
        L"レイヤー一括切り替え v2.0 forked by Garech"
    };

    struct HeaderHitTest {
        POINT point{};
        bool valid = false;
        bool is_header = false;
        bool enable = true;
        bool locked = false;
        int layer = 0;
        int probe_x = 0;
    };

    struct LayerHitTest {
        POINT point{};
        bool valid = false;
        int layer = 0;
    };

    struct ApplyLayers {
        bool enable = true;
        bool lock_mode = false;
        std::vector<int> layers;
    };


    // ロック切り替え操作用の修飾キー判定（Ctrl+Alt）
    bool HasLockModifiers() {
        return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
            (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 &&
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0;
    }

    // 表示切り替え操作用の修飾キー判定（Ctrl+Shift）
    bool HasVisibilityModifiers() {
        return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 &&
            (GetAsyncKeyState(VK_MENU) & 0x8000) == 0;
    }


    // レイヤー名領域がクリックされたかを判定
    void ReadHeaderHitTestImpl(void* param, EDIT_SECTION* edit) {
        auto& hit = *static_cast<HeaderHitTest*>(param);

        if (edit == nullptr || edit->info == nullptr) {
            return;
        }

        int clicked_layer = 0;
        int clicked_frame = 0;

        // 最初のクリック位置で座標変換できなければ、操作対象外とする
        if (!edit->pos_to_layer_frame(
            hit.point.x, hit.point.y, &clicked_layer, &clicked_frame)) {
            return;
        }


        hit.valid = true;
        hit.layer = clicked_layer;
        hit.probe_x = hit.point.x;
        hit.enable = edit->get_layer_enable(clicked_layer);
        hit.locked = edit->get_layer_lock(clicked_layer);

        // 先頭表示フレーム以下の場合のみ、レイヤーヘッダー操作とする
        hit.is_header = clicked_frame <= edit->info->display_frame_start;
    }

    // ReadHeaderHitTestImpl を呼び出すラッパー
    void ReadHeaderHitTest(void* param, EDIT_SECTION* edit) {
        auto* hit = static_cast<HeaderHitTest*>(param);
        // 初期状態は無効とする
        hit->valid = false;
        hit->is_header = false;
        __try {
        ReadHeaderHitTestImpl(param, edit);
    }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            hit->valid = false;
            hit->is_header = false;
            LogVerbose(L"レイヤー名領域の判定に失敗しました。");
        }
    }

    // 指定座標が有効なレイヤー位置かを判定
    void ReadLayerHitTestImpl(void* param, EDIT_SECTION* edit) {
        auto& hit = *static_cast<LayerHitTest*>(param);
        if (edit == nullptr || edit->info == nullptr) {
            return;
        }
        int frame = 0;
        hit.valid = edit->pos_to_layer_frame(hit.point.x, hit.point.y, &hit.layer, &frame);
    }

    // ReadLayerHitTestImpl を呼び出すラッパー
    void ReadLayerHitTest(void* param, EDIT_SECTION* edit) {
        auto* hit = static_cast<LayerHitTest*>(param);
        // 初期状態は無効とする
        hit->valid = false;
        __try {
        ReadLayerHitTestImpl(param, edit);
    }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            hit->valid = false;
            LogVerbose(L"レイヤー位置の判定に失敗しました。");
        }
    }

    // 実際にレイヤーの表示/ロック状態を切り替える
    void ApplyLayerVisibilityImpl(void* param, EDIT_SECTION* edit) {
        if (edit == nullptr || edit->info == nullptr) {
            return;
        }
        const auto& request = *static_cast<ApplyLayers*>(param);
        for (const int layer : request.layers) {
            if (request.lock_mode) {
                if (edit->get_layer_lock(layer) != request.enable) {
                    edit->set_layer_lock(layer, request.enable);
                }
            }
            else if (edit->get_layer_enable(layer) != request.enable) {
                edit->set_layer_enable(layer, request.enable);
            }
        }
    }

    // ApplyLayerVisibilityImpl を呼び出すラッパー（例外ハンドリングは行わない）
    void ApplyLayerVisibility(void* param, EDIT_SECTION* edit) {
        __try {
        ApplyLayerVisibilityImpl(param, edit);
    }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            LogVerbose(L"レイヤー状態の編集に失敗しました。");
        }
    }


    // マウスの Y 座標が変化する度に呼ばれる
    // ドラッグ中のレイヤー範囲を判定する
    void TrackMouseY(int screen_y) {
        if (!g_drag.active || g_edit_handle == nullptr) {
            return;
        }

        LayerHitTest hit{ {g_drag.probe_x, screen_y} };
        if (g_edit_handle->call_edit_section_param(&hit, ReadLayerHitTest) && hit.valid) {
            ApplyLayers request;
            request.enable = g_drag.enable;
            request.lock_mode = g_drag.lock_mode;
            // ドラッグ範囲全体を毎回収集して適用する（履歴を保持しない）
            int first = g_drag.last_layer;
            int last = hit.layer;
            if (first > last) std::swap(first, last);
            request.layers.clear();
            for (int layer = first; layer <= last; ++layer) {
                request.layers.push_back(layer);
            }

            if (!request.layers.empty() &&
                !g_edit_handle->call_edit_section_param(&request, ApplyLayerVisibility)) {
                return;
            }

            g_drag.last_layer = hit.layer;
        }
    }

    // ドラッグ操作を終了し、状態をリセットする
    void FinishDrag() {
        if (!g_drag.active) {
            return;
        }
        g_drag = {};
    }

    // ドラッグ操作を開始する時に呼ばれる
    // レイヤーの切り替え操作を行い、ドラッグ開始状態を保持する
    bool BeginDrag(const POINT point) {
        if (g_edit_handle == nullptr) {
            return false;
        }

        HeaderHitTest hit;
        hit.point = point;

        // レイヤー名領域がクリックされていない場合は、切替操作を行わない
        if (!g_edit_handle->call_edit_section_param(&hit, ReadHeaderHitTest) ||
            !hit.valid || !hit.is_header) {
            return false;
        }

        g_drag.active = true;
        g_drag.probe_x = hit.probe_x;
        g_drag.last_layer = hit.layer;
        g_drag.lock_mode = HasLockModifiers();
        g_drag.enable = g_drag.lock_mode ? !hit.locked : !hit.enable;


        ApplyLayers request;
        request.enable = g_drag.enable;
        request.lock_mode = g_drag.lock_mode;
        request.layers.push_back(hit.layer);
        if (!g_edit_handle->call_edit_section_param(&request, ApplyLayerVisibility)) {
            g_drag = {};
            return false;
        }


        return true;
    }

    // ドラッグ操作を制御する用のウィンドウプロシージャ
    LRESULT CALLBACK MessageWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        const POINT point{
            static_cast<LONG>(static_cast<LONG_PTR>(wparam)),
            static_cast<LONG>(static_cast<LONG_PTR>(lparam))
        };

        switch (message) {
        case kMessageDragStart:
            if (g_drag.active) {
                FinishDrag();
            }
            if (!BeginDrag(point) && g_input_capture) {
                // 操作対象外なら、抑止した左ボタン押下を再送して通常操作を行う。
                //INPUT input{};
                //input.type = INPUT_MOUSE;
                //input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                //SendInput(1, &input, sizeof(input));
                //g_input_capture = false;
                return DefWindowProcW(window, message, wparam, lparam);
            }
            return 0;

        case kMessageDragMove:
            if (g_drag.active) {
                TrackMouseY(point.y);
            }
            return 0;

        case kMessageDragFinish:
            if (g_drag.active) {
                TrackMouseY(point.y);
                FinishDrag();
            }
            return 0;
        }

        return DefWindowProcW(window, message, wparam, lparam);
    }

    // ローレベルマウスフックプロシージャ
    // 修飾キーが押されている場合はメッセージウィンドウへ操作を転送する
    LRESULT CALLBACK MouseHookProc(int code, WPARAM wparam, LPARAM lparam) {
        if (code < HC_ACTION) {
            return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
        }

        const auto& mouse = *reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
        // フォールバックで再送したマウス入力を再処理しない
        //if ((mouse.flags & LLMHF_INJECTED) != 0) {
        //    return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
        //}
        const UINT message = static_cast<UINT>(wparam);

        if (message == WM_LBUTTONDOWN) {
            // クリック位置のウィンドウを取得し、同一プロセスのウィンドウであれば処理を開始する
            const HWND target_window = WindowFromPoint(mouse.pt);
            if (g_message_window != nullptr) {
                DWORD process_id = 0;
                GetWindowThreadProcessId(target_window, &process_id);

                // 現プロセスのウィンドウかつ修飾キーが押されている場合のみ開始
                if (process_id == GetCurrentProcessId() && (HasVisibilityModifiers() || HasLockModifiers())) {
                    if (PostMessageW(g_message_window, kMessageDragStart,
                        static_cast<WPARAM>(mouse.pt.x), static_cast<LPARAM>(mouse.pt.y))) {
                        g_input_capture = true;
                        return 1;
                    }
                }
            }
        }
        else if (g_input_capture && message == WM_MOUSEMOVE) {
            // ドラッグは観察し続けるが、マウス移動メッセージ自体は奪わない。
            // ローレベルフックで WM_MOUSEMOVE をブロックすると、システムカーソルの
            // 位置更新が止まり、ポインタが固まって見えるためである。
            if (!PostMessageW(g_message_window, kMessageDragMove,
                static_cast<WPARAM>(mouse.pt.x), static_cast<LPARAM>(mouse.pt.y))) {
                g_input_capture = false;
            }
        }
        else if (g_input_capture && message == WM_LBUTTONUP) {
            g_input_capture = false;
            if (PostMessageW(g_message_window, kMessageDragFinish,
                static_cast<WPARAM>(mouse.pt.x), static_cast<LPARAM>(mouse.pt.y))) {
                return 1;
            }
        }

        return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
    }

} // namespace

// DLL のエントリポイント
// DLL_PROCESS_ATTACH でインスタンスハンドルを保存し、スレッド通知を無効化する
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_instance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

// logger2 初期化コールバック（ホストが呼ぶ）
EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* logger) {
    g_logger = logger;
}

EXTERN_C __declspec(dllexport) DWORD RequiredVersion() {
    return kRequiredVersion;
}

EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD) {
    return true;
}

// 終了処理　登録したフックやウィンドウなどを解放する
EXTERN_C __declspec(dllexport) void UninitializePlugin() {
    if (g_mouse_hook != nullptr) {
        UnhookWindowsHookEx(g_mouse_hook);
        g_mouse_hook = nullptr;
    }

    g_input_capture = false;
    g_drag = {};
    if (g_message_window != nullptr) {
        DestroyWindow(g_message_window);
        g_message_window = nullptr;
    }
    UnregisterClassW(kMessageWindowClass, g_instance);
    g_edit_handle = nullptr;
}

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable() {
    return &g_plugin_table;
}

// 登録処理　エディットハンドルやメッセージ用ウィンドウ、マウスフックを初期化する
EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
    // ホストテーブルを保存しておく
    g_host = host;
    host->set_plugin_information(g_plugin_table.information);

    g_edit_handle = host->create_edit_handle();
    if (g_edit_handle == nullptr) {
        OutputDebugStringW(L"toggle_layers_exedit2: create_edit_handle failed.\n");
        return;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = g_instance;
    window_class.lpfnWndProc = MessageWindowProc;
    window_class.lpszClassName = kMessageWindowClass;
    if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        OutputDebugStringW(L"toggle_layers_exedit2: RegisterClassExW failed.\n");
        return;
    }

    g_message_window = CreateWindowExW(
        0, kMessageWindowClass, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, g_instance, nullptr);
    if (g_message_window == nullptr) {
        OutputDebugStringW(L"toggle_layers_exedit2: CreateWindowExW failed.\n");
        return;
    }

    // フック内ではメッセージを送るだけにし、SDKの処理は後で実行する
    g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, g_instance, 0);
    if (g_mouse_hook == nullptr) {
        OutputDebugStringW(L"toggle_layers_exedit2: SetWindowsHookExW failed.\n");
    }
}

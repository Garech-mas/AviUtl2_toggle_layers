#include <windows.h>
#include <windows.h>
#include <algorithm>
#include <vector>
#include <string>
#include <unordered_map>

#include "plugin2.h"
#include "logger2.h"

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

	// マッピングキー: ベース文字列 + 修飾キーのフラグ
	struct DragKey {
		std::wstring base;
		bool ctrl = false;
		bool shift = false;
		bool alt = false;

		bool operator==(const DragKey& o) const noexcept {
			return base == o.base && ctrl == o.ctrl && shift == o.shift && alt == o.alt;
		}
	};

	struct DragKeyHash {
		size_t operator()(DragKey const& k) const noexcept {
			// simple hash: combine base hash and flag bits
			std::hash<std::wstring> h;
			size_t v = h(k.base);
			size_t f = (k.ctrl ? 1u : 0u) | (k.shift ? 2u : 0u) | (k.alt ? 4u : 0u);
			return v ^ (f + 0x9e3779b97f4a7c15ULL + (v << 6) + (v >> 2));
		}
	};

	std::unordered_map<DragKey, int, DragKeyHash> g_drag_map;

	// ボタンベース文字列
	constexpr std::wstring_view kBaseL = L"l";
	constexpr std::wstring_view kBaseR = L"r";
	constexpr std::wstring_view kBaseM = L"m";
	constexpr std::wstring_view kBaseX1 = L"x1";
	constexpr std::wstring_view kBaseX2 = L"x2";

	// 修飾キー判定・正規化用のヘルパ構造体
	struct Modifiers {
		bool ctrl = false;
		bool shift = false;
		bool alt = false;

		// 現在押されている修飾キーを取得
		static Modifiers FromCurrentKeys() {
			Modifiers m;
			m.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
			m.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
			m.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
			return m;
		}

		// ToSuffix を廃止：修飾はフラグで扱う

		// iniの修飾子部分（例: "alt+shift+ctrl"）を解析してフラグにする
		static Modifiers FromModifierString(const std::wstring& raw) {
			Modifiers m;
			std::wstring inner = raw;
			// split by '+'
			size_t start = 0;
			while (start < inner.size()) {
				size_t pos = inner.find(L'+', start);
				if (pos == std::wstring::npos) pos = inner.size();
				std::wstring token = inner.substr(start, pos - start);
				// trim
				size_t a = 0; while (a < token.size() && iswspace(token[a])) ++a;
				size_t b = token.size(); while (b > a && iswspace(token[b - 1])) --b;
				token = token.substr(a, b - a);
				for (auto& c : token) c = towlower(c);
				if (token == L"ctrl") m.ctrl = true;
				else if (token == L"shift") m.shift = true;
				else if (token == L"alt") m.alt = true;
				start = pos + 1;
			}
			return m;
		}

	};
	int g_last_button_code = 0; // 1=L,2=R,3=M,4=X1,5=X2
	bool g_drag_pending = false;
	POINT g_pending_point{};
	int g_pending_button_code = 0;

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

	// ini ファイルから設定をロード
	void LoadConfig() {
		g_drag_map.clear();
		wchar_t module_path[MAX_PATH]{ 0 };
		if (GetModuleFileNameW(g_instance, module_path, (DWORD)std::size(module_path)) == 0) {
			return;
		}

		std::wstring path(module_path);
		auto pos = path.find_last_of(L"\\/");
		std::wstring dir = (pos == std::wstring::npos) ? L"." : path.substr(0, pos);
		std::wstring ini = dir + L"\\toggle_layers.ini"; // ini 拡張子のファイルを使用

		const wchar_t* section = L"mouse_override.layer.drag";
		const DWORD bufsize = 32768;
		std::vector<wchar_t> buf(bufsize);
		DWORD read = GetPrivateProfileSectionW(section, buf.data(), bufsize, ini.c_str());
		if (read == 0) {
			return;
		}
		// バッファは複数のヌル終端文字列が連続しており、末尾は二重ヌルで終わる
		wchar_t* p = buf.data();
		while (*p != L'\0') {
			std::wstring line(p);
			// 次の文字列へ進める
			p += line.size() + 1;

			auto eq = line.find(L'=');
			if (eq == std::wstring::npos) {
				continue;
			}
			std::wstring key = line.substr(0, eq);
			std::wstring val = line.substr(eq + 1);

			// key/val の前後空白をトリム
			auto trim = [](std::wstring& s) {
				size_t a = 0;
				while (a < s.size() && iswspace(s[a])) ++a;
				size_t b = s.size();
				while (b > a && iswspace(s[b - 1])) --b;
				s = s.substr(a, b - a);
				};
			trim(key);
			trim(val);

			// キーを正規化（小文字化、修飾子順序を標準化）
			for (auto& c : key) c = towlower(c);
			size_t lt = key.find(L'<');
			size_t gt = key.find_last_of(L'>');
			std::wstring base = key;
			Modifiers m;
			if (lt != std::wstring::npos && gt != std::wstring::npos && gt > lt) {
				base = key.substr(0, lt);
				std::wstring inner = key.substr(lt + 1, gt - lt - 1);
				m = Modifiers::FromModifierString(inner);
			}
			// trim base
			size_t a = 0; while (a < base.size() && iswspace(base[a])) ++a;
			size_t b = base.size(); while (b > a && iswspace(base[b - 1])) --b;
			base = base.substr(a, b - a);

			DragKey dk{ base, m.ctrl, m.shift, m.alt };
			int ival = _wtoi(val.c_str());
			g_drag_map.emplace(dk, ival);
		}
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
	// マッピングの値:
	// 0 = 割り当てなし（操作しない：DefWindowProcW に回す）
	// 1 = レイヤーの表示をトグル（ON/OFF を切り替え）
	// 2 = レイヤーのロックをトグル（ON/OFF を切り替え）
	bool BeginDrag(const POINT point, int button_code) {
		if (g_edit_handle == nullptr) {
			return false;
		}

		HeaderHitTest hit;
		hit.point = point;

		// レイヤー名領域がクリックされていない場合は、切替操作を行わない
		if (!g_edit_handle->call_edit_section_param(&hit, ReadHeaderHitTest) ||
			!hit.valid || !hit.is_header) {
			LogVerbose(L"BeginDrag: header hit test failed (valid=%d is_header=%d)", hit.valid ? 1 : 0, hit.is_header ? 1 : 0);
			return false;
		}

		// ボタンに応じたベース文字列（小文字の定数を使用）
		std::wstring_view base = kBaseL;
		switch (button_code) {
		case 1: base = kBaseL; break;
		case 2: base = kBaseR; break;
		case 3: base = kBaseM; break;
		case 4: base = kBaseX1; break;
		case 5: base = kBaseX2; break;
		default: base = kBaseL; break;
		}

		// ボタンと現在の修飾キーから検索キーを作成
		Modifiers mods = Modifiers::FromCurrentKeys();
		DragKey lookup{ std::wstring(base), mods.ctrl, mods.shift, mods.alt };

		int code = -1;
		auto it = g_drag_map.find(lookup);
		// 修飾キーが無い場合のみベースでのフォールバックを行う
		if (it == g_drag_map.end() && !mods.ctrl && !mods.shift && !mods.alt) {
			DragKey basekey{ std::wstring(base), false, false, false };
			it = g_drag_map.find(basekey);
		}

		if (it != g_drag_map.end()) {
			code = it->second;
		}
		else {
			// マッピング未指定は 0 (割り当てなし) として扱う
			code = 0;
		}

		bool final_lock_mode = false;
		bool final_enable = false;

		if (code >= 0) {
			// mapping exists
			if (code == 0) {
				// explicit no-assignment: do not start special handling
				return false;
			}
			else if (code == 1) {
				final_lock_mode = false;
				final_enable = !hit.enable; // toggle visibility
			}
			else if (code == 2) {
				final_lock_mode = true;
				final_enable = !hit.locked; // toggle lock
			}
			else {
				// unknown value: デフォルトは表示トグルとする
				final_lock_mode = false;
				final_enable = !hit.enable;
			}
		}
		else {
			// no mapping found: デフォルトは表示トグルとする
			final_lock_mode = false;
			final_enable = !hit.enable;
		}

		g_drag.active = true;
		g_drag.probe_x = hit.probe_x;
		g_drag.last_layer = hit.layer;
		g_drag.lock_mode = final_lock_mode;
		g_drag.enable = final_enable;


		ApplyLayers request;
		request.enable = g_drag.enable;
		request.lock_mode = g_drag.lock_mode;
		request.layers.push_back(hit.layer);
		if (!g_edit_handle->call_edit_section_param(&request, ApplyLayerVisibility)) {
			LogVerbose(L"BeginDrag: ApplyLayerVisibility failed for initial layer %d", hit.layer);
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
			// 実際にマウスが動くまで操作開始を遅延する
			g_drag_pending = true;
			g_pending_point = point;
			g_pending_button_code = g_last_button_code;
			return 0;

		case kMessageDragMove:
			if (g_drag_pending) {
				// 押下後の最初の移動時：マッピングが許可するならドラッグを開始する
				if (BeginDrag(g_pending_point, g_pending_button_code)) {
					// 開始成功: 現在位置で処理
					TrackMouseY(point.y);
				}
				else {
					// not handled: 再送して通常クリックにする
					INPUT input{};
					input.type = INPUT_MOUSE;
					switch (g_pending_button_code) {
					case 1: input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; break;
					case 2: input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; break;
					case 3: input.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN; break;
					case 4: input.mi.dwFlags = MOUSEEVENTF_XDOWN; input.mi.mouseData = XBUTTON1 << 16; break;
					case 5: input.mi.dwFlags = MOUSEEVENTF_XDOWN; input.mi.mouseData = XBUTTON2 << 16; break;
					default: input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; break;
					}
					SendInput(1, &input, sizeof(INPUT));

					// キャプチャを解除して以降のイベントを通常処理へ戻す
					g_input_capture = false;
				}

				g_drag_pending = false;
			}
			else if (g_drag.active) {
				TrackMouseY(point.y);
			}
			return 0;

		case kMessageDragFinish:
			if (g_drag_pending) {
				// マウスが移動せずに離された: クリックとして再送 (down -> up)
				INPUT inputs[2]{};
				inputs[0].type = INPUT_MOUSE; inputs[1].type = INPUT_MOUSE;
				switch (g_pending_button_code) {
				case 1:
					inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP; break;
				case 2:
					inputs[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; inputs[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP; break;
				case 3:
					inputs[0].mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN; inputs[1].mi.dwFlags = MOUSEEVENTF_MIDDLEUP; break;
				case 4:
					inputs[0].mi.dwFlags = MOUSEEVENTF_XDOWN; inputs[0].mi.mouseData = XBUTTON1 << 16;
					inputs[1].mi.dwFlags = MOUSEEVENTF_XUP; inputs[1].mi.mouseData = XBUTTON1 << 16; break;
				case 5:
					inputs[0].mi.dwFlags = MOUSEEVENTF_XDOWN; inputs[0].mi.mouseData = XBUTTON2 << 16;
					inputs[1].mi.dwFlags = MOUSEEVENTF_XUP; inputs[1].mi.mouseData = XBUTTON2 << 16; break;
				default:
					inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP; break;
				}
				SendInput(2, inputs, sizeof(INPUT));
				g_drag_pending = false;
				g_input_capture = false;
			}
			else if (g_drag.active) {
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
		if ((mouse.flags & LLMHF_INJECTED) != 0) {
			return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
		}
		const UINT message = static_cast<UINT>(wparam);

		// 各ボタンのダウンイベントを処理 (L, R, M, X1/X2)
		if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN) {
			int button_code = 0;
			if (message == WM_LBUTTONDOWN) button_code = 1;
			else if (message == WM_RBUTTONDOWN) button_code = 2;
			else if (message == WM_MBUTTONDOWN) button_code = 3;
			else if (message == WM_XBUTTONDOWN) {
				const DWORD which = HIWORD(mouse.mouseData);
				button_code = (which == XBUTTON1) ? 4 : 5;
			}

			// クリック位置のウィンドウを取得し、同一プロセスのウィンドウであれば処理を開始する
			const HWND target_window = WindowFromPoint(mouse.pt);
			if (g_message_window != nullptr) {
				DWORD process_id = 0;
				GetWindowThreadProcessId(target_window, &process_id);

				// マッピングが設定されているかに基づいて開始判定する
				if (process_id == GetCurrentProcessId()) {
					// BeginDrag と同様にベース文字列を定義（定数利用）
					std::wstring_view base = kBaseL;
					switch (button_code) {
					case 1: base = kBaseL; break;
					case 2: base = kBaseR; break;
					case 3: base = kBaseM; break;
					case 4: base = kBaseX1; break;
					case 5: base = kBaseX2; break;
					default: base = kBaseL; break;
					}

					Modifiers mods = Modifiers::FromCurrentKeys();
					DragKey lookup{ std::wstring(base), mods.ctrl, mods.shift, mods.alt };

					// ルックアップ
					auto it = g_drag_map.find(lookup);
					// 修飾キーが無い場合のみベースでのフォールバックを行う
					if (it == g_drag_map.end() && !mods.ctrl && !mods.shift && !mods.alt) {
						DragKey basekey{ std::wstring(base), false, false, false };
						it = g_drag_map.find(basekey);
					}

					if (it == g_drag_map.end()) {
						// マッピング無し -> 通常処理へ委ねる
					}
					else {
						int code = it->second;
						if (code == 0) {
							// 明示的に割り当て無し -> 処理しない
						}
						else {
							// ヘッダーがクリックされたか確認する
							if (g_edit_handle != nullptr) {
								HeaderHitTest hit;
								hit.point = mouse.pt;
								if (g_edit_handle->call_edit_section_param(&hit, ReadHeaderHitTest) && hit.valid && hit.is_header) {
									// ヘッダーがクリックされた -> キャプチャ
									g_last_button_code = button_code;
									if (PostMessageW(g_message_window, kMessageDragStart,
										static_cast<WPARAM>(mouse.pt.x), static_cast<LPARAM>(mouse.pt.y))) {
										g_input_capture = true;
										return 1;
									}
									else {
										LogVerbose(L"MouseHookProc: failed to post kMessageDragStart");
									}
								}
								else {
									LogVerbose(L"MouseHookProc: header hit test failed or not header (valid=%d is_header=%d)", hit.valid ? 1 : 0, hit.is_header ? 1 : 0);
								}
							}
							else {
								LogVerbose(L"MouseHookProc: g_edit_handle is null -> not capturing");
							}
						}
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
		else if (g_input_capture && (message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP || message == WM_XBUTTONUP)) {
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

	// ini から設定をロード
	LoadConfig();

	// フック内ではメッセージを送るだけにし、SDKの処理は後で実行する
	g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, g_instance, 0);
	if (g_mouse_hook == nullptr) {
		OutputDebugStringW(L"toggle_layers_exedit2: SetWindowsHookExW failed.\n");
	}
}

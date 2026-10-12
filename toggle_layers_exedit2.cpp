#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include "plugin2.h"
#include "config2.h"
#include "logger2.h"
#include "resource.h"

namespace {

	constexpr DWORD kRequiredVersion = 2004500; // 必須バージョン: beta45
	constexpr wchar_t kIniFileName[] = L"toggle_layers.ini";
	constexpr wchar_t kIniSection[] = L"mouse_override.layer.drag";

	HINSTANCE g_instance = nullptr;
	CONFIG_HANDLE* g_config = nullptr;
	LOG_HANDLE* g_logger = nullptr;
	EDIT_HANDLE* g_edit_handle = nullptr;
	HWND g_host_window = nullptr;
	HHOOK g_mouse_hook = nullptr;
	int g_replay_remaining = 0;               // SendInput で再送した、まだ届いていないボタンイベントの数
	int g_hook_depth = 0;                     // フックプロシージャの入れ子の深さ (再入ガード用)
	std::atomic<bool> g_host_ready{ false };  // ホストが API を受け付けられる状態になったか (プロジェクトのロード後)

	COMMON_PLUGIN_TABLE g_plugin_table = {
		L"レイヤー一括切り替え",
		L"レイヤー一括切り替え v2.3 ported by Garech"
	};

	// ------------------------------------------------------------------
	// ログ
	// ------------------------------------------------------------------

	enum class LogLevel { Verbose, Warn, Error };

	// ログ出力。バッファを超える場合は切り詰める (ログは 1024 文字で制限される)
	void Log(LogLevel level, const wchar_t* fmt, ...) {
		if (g_logger == nullptr) {
			return;
		}
		wchar_t buf[1024];
		va_list ap;
		va_start(ap, fmt);
		_vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, ap);
		va_end(ap);

		void (*write)(LOG_HANDLE*, LPCWSTR) = nullptr;
		switch (level) {
		case LogLevel::Verbose: write = g_logger->verbose; break;
		case LogLevel::Warn: write = g_logger->warn; break;
		case LogLevel::Error: write = g_logger->error; break;
		}
		if (write != nullptr) {
			write(g_logger, buf);
		}
	}

	// ------------------------------------------------------------------
	// ボタン・修飾キー・割り当て
	// ------------------------------------------------------------------

	enum class Button : int { L, R, M, X1, X2, Count };
	constexpr size_t kButtonCount = static_cast<size_t>(Button::Count);

	// 修飾キーのビットフラグ
	constexpr unsigned kModCtrl = 1;
	constexpr unsigned kModShift = 2;
	constexpr unsigned kModAlt = 4;
	constexpr size_t kModCombos = 8;

	// ini の値: 0 = 割り当てなし(通常動作), 1 = 表示のトグル, 2 = ロックのトグル
	enum class Action : int { None = 0, ToggleVisible = 1, ToggleLock = 2 };

	// [ボタン][修飾キーのビット] -> 動作。未指定は None (0) になる
	std::array<std::array<Action, kModCombos>, kButtonCount> g_actions{};
	size_t g_action_count = 0;

	unsigned CurrentModifiers() {
		unsigned mods = 0;
		if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= kModCtrl;
		if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= kModShift;
		if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= kModAlt;
		return mods;
	}

	void Trim(std::wstring& s) {
		size_t a = 0;
		while (a < s.size() && iswspace(s[a])) ++a;
		size_t b = s.size();
		while (b > a && iswspace(s[b - 1])) --b;
		s = s.substr(a, b - a);
	}

	void ToLower(std::wstring& s) {
		for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
	}

	// "alt+shift+ctrl" のような修飾子部分をビットフラグにする
	unsigned ParseModifiers(const std::wstring& inner) {
		unsigned mods = 0;
		size_t start = 0;
		for (;;) {
			size_t pos = inner.find(L'+', start);
			if (pos == std::wstring::npos) pos = inner.size();
			std::wstring token = inner.substr(start, pos - start);
			Trim(token);
			ToLower(token);
			if (token == L"ctrl") mods |= kModCtrl;
			else if (token == L"shift") mods |= kModShift;
			else if (token == L"alt") mods |= kModAlt;
			else if (!token.empty()) Log(LogLevel::Warn, L"ini: 不明な修飾キー '%ls' を無視しました。", token.c_str());
			if (pos >= inner.size()) break;
			start = pos + 1;
		}
		return mods;
	}

	bool ParseButton(const std::wstring& s, Button* out) {
		if (s == L"l") *out = Button::L;
		else if (s == L"r") *out = Button::R;
		else if (s == L"m") *out = Button::M;
		else if (s == L"x1") *out = Button::X1;
		else if (s == L"x2") *out = Button::X2;
		else return false;
		return true;
	}

	Action ParseAction(int value) {
		if (value == 1) return Action::ToggleVisible;
		if (value == 2) return Action::ToggleLock;
		return Action::None;
	}

	// ini ファイルから設定をロードする
	void LoadConfig() {
		g_actions = {};
		g_action_count = 0;

		wchar_t module_path[MAX_PATH]{};
		const DWORD len = GetModuleFileNameW(g_instance, module_path, ARRAYSIZE(module_path));
		if (len == 0 || len >= ARRAYSIZE(module_path)) {
			Log(LogLevel::Error, L"DLL のパスを取得できませんでした。");
			return;
		}
		const std::wstring path(module_path, len);
		const size_t slash = path.find_last_of(L"\\/");
		const std::wstring dir = (slash == std::wstring::npos) ? std::wstring(L".") : path.substr(0, slash);
		const std::wstring ini_path = dir + L"\\" + kIniFileName;

		// 既に設定ファイルが存在する場合はそれを使う
		if (GetFileAttributesW(ini_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
			Log(LogLevel::Verbose, L"既存の設定ファイルを使用します: %ls", ini_path.c_str());
		}
		else {
			// 設定ファイルがない場合はリソースから展開する
			HRSRC hres = FindResourceW(g_instance, MAKEINTRESOURCEW(IDR_TOGGLE_LAYERS_INI), RT_RCDATA);
			if (hres != nullptr) {
				HGLOBAL hglob = LoadResource(g_instance, hres);
				if (hglob != nullptr) {
					void* data = LockResource(hglob);
					const DWORD size = SizeofResource(g_instance, hres);
					if (data != nullptr && size > 0) {
						HANDLE fh = CreateFileW(ini_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
						if (fh != INVALID_HANDLE_VALUE) {
							DWORD written = 0;
							if (WriteFile(fh, data, size, &written, nullptr) && written == size) {
								Log(LogLevel::Verbose, L"埋め込み設定ファイルを展開しました: %ls", ini_path.c_str());
							}
							else {
								Log(LogLevel::Warn, L"埋め込み設定ファイルの書き出しに失敗しました: %ls (err=%lu)", ini_path.c_str(), GetLastError());
							}
							CloseHandle(fh);
						}
					}
				}
			}
		}

		constexpr DWORD kBufSize = 32768;
		std::vector<wchar_t> buf(kBufSize);
		if (GetPrivateProfileSectionW(kIniSection, buf.data(), kBufSize, ini_path.c_str()) == 0) {
			Log(LogLevel::Warn, L"ini: セクション [%ls] がないか空です。", kIniSection);
			return;
		}

		// バッファは複数のヌル終端文字列が連続しており、末尾は二重ヌルで終わる
		for (const wchar_t* p = buf.data(); *p != L'\0'; p += wcslen(p) + 1) {
			const std::wstring line(p);
			const size_t eq = line.find(L'=');
			if (eq == std::wstring::npos) {
				continue;
			}
			std::wstring key = line.substr(0, eq);
			std::wstring val = line.substr(eq + 1);
			Trim(key);
			Trim(val);
			ToLower(key);

			// "l<ctrl+shift>" 形式: ベース文字列 + 修飾キー
			std::wstring base = key;
			unsigned mods = 0;
			const size_t lt = key.find(L'<');
			const size_t gt = key.find_last_of(L'>');
			if (lt != std::wstring::npos && gt != std::wstring::npos && gt > lt) {
				base = key.substr(0, lt);
				mods = ParseModifiers(key.substr(lt + 1, gt - lt - 1));
			}
			Trim(base);

			Button button;
			if (!ParseButton(base, &button)) {
				continue;
			}

			const int value = _wtoi(val.c_str());
			const Action action = ParseAction(value);
			g_actions[static_cast<size_t>(button)][mods] = action;
			if (action != Action::None) {
				++g_action_count;
			}
		}

		Log(LogLevel::Verbose, L"設定を読み込みました (割り当て %zu 件): %ls", g_action_count, ini_path.c_str());
	}


	// ------------------------------------------------------------------
	// 編集セクション内の処理
	// ------------------------------------------------------------------

	// 編集セクションのコールバック内で起きた例外を握りつぶし、コードをログに残す
	// (__try を使う関数にはデストラクタを持つオブジェクトを置けないので、中身は持たせない)
	template <void (*Impl)(void*, EDIT_SECTION*)>
	void SafeEdit(void* param, EDIT_SECTION* edit) {
		__try {
			Impl(param, edit);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			Log(LogLevel::Error, L"編集セクション内で例外が発生しました (code=0x%08lX)", GetExceptionCode());
		}
	}

	// 画面X座標 x のフレーム番号 (レイヤー編集上でなければ false)
	bool FrameAtX(EDIT_SECTION* edit, int x, int y, int* frame) {
		int layer = 0;
		if (!edit->pos_to_layer_frame(x, y, &layer, frame)) {
			return false;
		}
		if (layer < 0 || (frame != nullptr && *frame < 0)) {
			return false;
		}
		return true;
	}

	// x 以降で、フレーム番号が target 以上になる最小のX座標を探す (右へ向かって単調増加と仮定)
	bool FindFrameStartX(EDIT_SECTION* edit, int x, int y, int limit, int target, int* found) {
		int frame = 0;
		if (!FrameAtX(edit, x, y, &frame)) {
			return false;
		}
		if (frame >= target) {
			*found = x;
			return true;
		}

		// 指数探索で上限を見つけてから二分探索する。lo は frame < target、hi は frame >= target
		int lo = x;
		int hi = x;
		for (int step = 1;; step *= 2) {
			hi = (std::min)(x + step, limit);
			if (hi <= lo || !FrameAtX(edit, hi, y, &frame)) {
				return false;
			}
			if (frame >= target) {
				break;
			}
			lo = hi;
		}
		while (hi - lo > 1) {
			const int mid = lo + (hi - lo) / 2;
			if (!FrameAtX(edit, mid, y, &frame)) {
				return false;
			}
			if (frame >= target) {
				hi = mid;
			}
			else {
				lo = mid;
			}
		}
		*found = hi;
		return true;
	}

	// クリック位置がレイヤーヘッダー(左側のレイヤー名領域)かを判定する
	bool IsLayerHeader(EDIT_SECTION* edit, int x, int y, int frame) {
		const int start = edit->info->display_frame_start;
		if (frame < start) return true;
		if (frame > start) return false;

		// frame == start のとき、ヘッダーと「先頭表示フレームのセル」は同じフレーム番号になり得る。
		// 先頭セルの左端(=ヘッダーの右端)を、次のフレーム境界とセル幅から逆算して区別する。
		const int limit = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1;
		int x1 = 0;
		int x2 = 0;
		if (!FindFrameStartX(edit, x, y, limit, start + 1, &x1) ||
			!FindFrameStartX(edit, x1, y, limit, start + 2, &x2)) {
			return true; // 境界を求められなければ、ヘッダー扱いにする
		}
		const int content_left = x1 - (x2 - x1);
		return x < content_left;
	}

	struct HeaderQuery {
		POINT point{};
		bool hit = false;    // レイヤーヘッダー上か
		bool enable = true;
		bool locked = false;
		int layer = 0;
	};

	// レイヤーヘッダー上がクリックされたかと、そのレイヤーの状態を取得する
	void HeaderHitTestImpl(void* param, EDIT_SECTION* edit) {
		auto& q = *static_cast<HeaderQuery*>(param);
		if (edit == nullptr || edit->info == nullptr) {
			return;
		}
		int layer = 0;
		int frame = 0;
		if (!edit->pos_to_layer_frame(q.point.x, q.point.y, &layer, &frame)) {
			return;
		}
		if (layer < 0 || frame < 0) {
			return;
		}
		q.layer = layer;
		q.enable = edit->get_layer_enable(layer);
		q.locked = edit->get_layer_lock(layer);
		q.hit = IsLayerHeader(edit, q.point.x, q.point.y, frame);
	}

	struct TrackRequest {
		int x = 0;           // 判定に使うスクリーン座標 (X は押下位置で固定)
		int y = 0;
		int last_layer = 0;  // 入出力: 最後に反映したレイヤー
		bool first = true;   // 最初の1回は、同じレイヤーでも反映する
		bool enable = true;
		bool lock_mode = false;
	};

	// 現在位置までのレイヤー範囲に、表示/ロック状態を反映する
	void TrackImpl(void* param, EDIT_SECTION* edit) {
		auto& r = *static_cast<TrackRequest*>(param);
		if (edit == nullptr || edit->info == nullptr) {
			return;
		}

		// 位置を取れなかった場合は、最後のレイヤーのままとして扱う
		int layer = r.last_layer;
		int hit = 0;
		int frame = 0;
		if (edit->pos_to_layer_frame(r.x, r.y, &hit, &frame) && hit >= 0 && frame >= 0) {
			layer = hit;
		}
		if (!r.first && layer == r.last_layer) {
			return;
		}

		const int lo = (std::min)(r.last_layer, layer);
		const int hi = (std::max)(r.last_layer, layer);
		for (int l = lo; l <= hi; ++l) {
			if (r.lock_mode) {
				if (edit->get_layer_lock(l) != r.enable) {
					edit->set_layer_lock(l, r.enable);
				}
			}
			else if (edit->get_layer_enable(l) != r.enable) {
				edit->set_layer_enable(l, r.enable);
			}
		}
		r.last_layer = layer;
	}

	// ------------------------------------------------------------------
	// ドラッグ操作
	// ------------------------------------------------------------------

	// 押下を奪ってから離すまでの状態。
	// 押下時にすべて決め、最初にマウスが動いた時点で切り替えを始める。
	// 動かずに離された場合は、押下を通常のクリックとして再送する。
	struct Gesture {
		bool active = false;
		bool armed = false;    // ドラッグと見なせる距離まで動いたか (false のままなら単なるクリック)
		bool started = false;  // 切り替えを開始したか
		Button button = Button::L;
		POINT down{};
		int last_y = 0;
		HWND capture = nullptr;
		TrackRequest track;
	};

	Gesture g_gesture;

	// ボタンの入れ替え設定を考慮して、論理ボタンに対応する物理ボタンの VK を返す
	int VirtualKeyOf(Button button) {
		const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
		switch (button) {
		case Button::L: return swapped ? VK_RBUTTON : VK_LBUTTON;
		case Button::R: return swapped ? VK_LBUTTON : VK_RBUTTON;
		case Button::M: return VK_MBUTTON;
		case Button::X1: return VK_XBUTTON1;
		default: return VK_XBUTTON2;
		}
	}

	bool IsButtonDown(Button button) {
		return (GetAsyncKeyState(VirtualKeyOf(button)) & 0x8000) != 0;
	}

	void ResetGesture() {
		if (g_gesture.capture != nullptr && GetCapture() == g_gesture.capture) {
			ReleaseCapture();
		}
		g_gesture = {};
	}

	// ボタンの押下と離上を、元の位置のまま再送して通常のクリックにする
	void ReplayClick(Button button) {
		DWORD down_flag = 0;
		DWORD up_flag = 0;
		DWORD data = 0;
		switch (VirtualKeyOf(button)) {
		case VK_LBUTTON: down_flag = MOUSEEVENTF_LEFTDOWN; up_flag = MOUSEEVENTF_LEFTUP; break;
		case VK_RBUTTON: down_flag = MOUSEEVENTF_RIGHTDOWN; up_flag = MOUSEEVENTF_RIGHTUP; break;
		case VK_MBUTTON: down_flag = MOUSEEVENTF_MIDDLEDOWN; up_flag = MOUSEEVENTF_MIDDLEUP; break;
		case VK_XBUTTON1: down_flag = MOUSEEVENTF_XDOWN; up_flag = MOUSEEVENTF_XUP; data = XBUTTON1; break;
		default: down_flag = MOUSEEVENTF_XDOWN; up_flag = MOUSEEVENTF_XUP; data = XBUTTON2; break;
		}

		INPUT inputs[2]{};
		inputs[0].type = INPUT_MOUSE;
		inputs[0].mi.dwFlags = down_flag;
		inputs[0].mi.mouseData = data;
		inputs[1].type = INPUT_MOUSE;
		inputs[1].mi.dwFlags = up_flag;
		inputs[1].mi.mouseData = data;

		// 再送したイベントは、フックを素通しさせる (カーソルは動いていないので座標指定は不要)
		g_replay_remaining = static_cast<int>(SendInput(2, inputs, sizeof(INPUT)));
	}

	// 本体の HWND、または分離されたレイヤー編集ウィンドウかを判定する
	bool IsTimelineWindow(HWND hwnd) {
		if (hwnd == nullptr) {
			return false;
		}

		// 本体の HWND 自身は対象とする
		if (hwnd == g_host_window) {
			return true;
		}

		// 同じプロセス内のウィンドウで、キャプションが「レイヤー編集」なら対象とする
		const HWND root = GetAncestor(hwnd, GA_ROOT);
		if (root == nullptr || root == g_host_window) {
			return false;
		}

		DWORD root_pid = 0;
		DWORD host_pid = 0;
		GetWindowThreadProcessId(root, &root_pid);
		if (g_host_window == nullptr ||
			GetWindowThreadProcessId(g_host_window, &host_pid) == 0 || root_pid != host_pid) {
			return false;
		}

		wchar_t caption[256]{};
		const int length = GetWindowTextW(root, caption, ARRAYSIZE(caption));
		if (length <= 0) {
			return false;
		}

		constexpr LPCWSTR kOriginalCaption = L"レイヤー編集";
		if (wcscmp(caption, kOriginalCaption) == 0) {
			return true;
		}

		if (g_config != nullptr && g_config->get_language_text != nullptr) {
			const LPCWSTR translated = g_config->get_language_text(
				g_config, L"Menu", kOriginalCaption);
			if (translated != nullptr && wcscmp(caption, translated) == 0) {
				return true;
			}
		}

		return false;
	}

	// 押下位置がレイヤーヘッダーで、割り当てがあれば、押下を奪って追跡を始める
	bool BeginGesture(Button button, const MOUSEHOOKSTRUCTEX& mh) {
		// タイムラインウィンドウ以外では割り当て判定を行わない
		if (!IsTimelineWindow(mh.hwnd)) {
			return false;
		}
		const Action action = g_actions[static_cast<size_t>(button)][CurrentModifiers()];
		if (action == Action::None) {
			return false;
		}

		// プロジェクトのロード通知が来るまで (起動処理中) は、ホストの API を一切呼ばず、通常のクリックとして扱う。
		// この間に call_edit_section を呼ぶと、ホストがアクセス違反を起こして固まる。
		if (!g_host_ready.load()) {
			return false;
		}
		// 編集中以外は編集セクションを呼び出さない。
		if (g_edit_handle->get_edit_state() != EDIT_HANDLE::EDIT_STATE_EDIT) {
			return false;
		}

		HeaderQuery q;
		q.point = mh.pt;
		if (!g_edit_handle->call_edit_section_param(&q, SafeEdit<HeaderHitTestImpl>) || !q.hit) {
			return false;
		}

		Gesture& g = g_gesture;
		g = {};
		g.active = true;
		g.button = button;
		g.down = mh.pt;
		g.last_y = mh.pt.y;
		g.track.last_layer = q.layer;
		g.track.lock_mode = (action == Action::ToggleLock);
		g.track.enable = g.track.lock_mode ? !q.locked : !q.enable;

		// クリック直後はキャプチャしない。ドラッグ判定距離を超えた時点で取得する。
		g.capture = mh.hwnd;
		return true;
	}

	// マウス位置までを反映する。ドラッグと見なせる距離まで動くまでは何もしない
	// (クリック中の数pxのブレで切り替わらないよう、システムのドラッグ判定距離を使う)
	void StepGesture(POINT pt) {
		Gesture& g = g_gesture;
		if (!g.armed) {
			if (std::abs(pt.x - g.down.x) < GetSystemMetrics(SM_CXDRAG) &&
				std::abs(pt.y - g.down.y) < GetSystemMetrics(SM_CYDRAG)) {
				return;
			}
			g.armed = true;
			// クリックだけの操作や、ウィンドウのドッキング操作を妨げないよう、
			// 実際にドラッグと判定してからマウスキャプチャを取得する。
			if (g.capture != nullptr) {
				SetCapture(g.capture);
			}
		}
		if (g.started && pt.y == g.last_y) {
			return;
		}

		// 再生・出力中は編集セクションを呼び出さない。
		if (g_edit_handle->get_edit_state() != EDIT_HANDLE::EDIT_STATE_EDIT) {
			return;
		}

		g.track.x = g.down.x;
		g.track.y = pt.y;
		g.track.first = !g.started;
		// 失敗した場合は状態を進めず、次の移動で再試行する
		if (g_edit_handle->call_edit_section_param(&g.track, SafeEdit<TrackImpl>)) {
			g.started = true;
			g.last_y = pt.y;
		}
	}

	void EndGesture(POINT pt) {
		StepGesture(pt); // 最後の位置まで反映する
		const Gesture g = g_gesture;
		ResetGesture();

		// ドラッグにならなかった場合は、何も切り替えず、押下位置での通常のクリックとして再送する
		if (!g.armed) {
			if (pt.x != g.down.x || pt.y != g.down.y) {
				SetCursorPos(g.down.x, g.down.y);
			}
			ReplayClick(g.button);
		}
	}

	// ボタンメッセージをボタンと押下/離上に分解する
	bool DecodeButtonMessage(UINT message, const MOUSEHOOKSTRUCTEX& mh, Button* button, bool* is_down) {
		switch (message) {
		case WM_LBUTTONDOWN: *button = Button::L; *is_down = true; return true;
		case WM_LBUTTONUP: *button = Button::L; *is_down = false; return true;
		case WM_RBUTTONDOWN: *button = Button::R; *is_down = true; return true;
		case WM_RBUTTONUP: *button = Button::R; *is_down = false; return true;
		case WM_MBUTTONDOWN: *button = Button::M; *is_down = true; return true;
		case WM_MBUTTONUP: *button = Button::M; *is_down = false; return true;
		case WM_XBUTTONDOWN:
		case WM_XBUTTONUP:
			*button = (HIWORD(mh.mouseData) == XBUTTON1) ? Button::X1 : Button::X2;
			*is_down = (message == WM_XBUTTONDOWN);
			return true;
		}
		return false;
	}

	// ホストのスレッド限定のマウスフック
	// 他のプロセスや他のスレッドの入力には影響しない
	LRESULT CALLBACK MouseHookProc(int code, WPARAM wparam, LPARAM lparam) {
		// HC_NOREMOVE (PeekMessage で取り出されていない) の場合は、同じメッセージが再度来るので処理しない
		if (code != HC_ACTION) {
			return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
		}

		// 再入ガード: 編集セクション内などでメッセージが処理され、フックが入れ子で呼ばれた場合は、何もせず通す
		if (g_hook_depth > 0) {
			return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
		}
		struct DepthGuard {
			DepthGuard() { ++g_hook_depth; }
			~DepthGuard() { --g_hook_depth; }
		} depth_guard;

		const auto& mh = *reinterpret_cast<const MOUSEHOOKSTRUCTEX*>(lparam);
		const UINT message = static_cast<UINT>(wparam);

		// 移動は奪わない (奪うとカーソル位置の更新が止まる)
		if (message == WM_MOUSEMOVE) {
			if (g_gesture.active) {
				if (IsButtonDown(g_gesture.button)) {
					StepGesture(mh.pt);
				}
				else {
					EndGesture(mh.pt); // 離上を取り逃していた場合の安全弁
				}
			}
			return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
		}

		Button button;
		bool is_down;
		if (!DecodeButtonMessage(message, mh, &button, &is_down)) {
			return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
		}

		// 自分で再送したクリックは処理しない
		if (g_replay_remaining > 0) {
			--g_replay_remaining;
			return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
		}

		if (g_gesture.active) {
			if (button != g_gesture.button) {
				return CallNextHookEx(g_mouse_hook, code, wparam, lparam); // 他のボタンには干渉しない
			}
			if (!is_down) {
				EndGesture(mh.pt);
				return 1;
			}
			// 離上を取り逃したまま次の押下が来た場合は、古い状態を捨てて新規として扱う
			ResetGesture();
		}

		if (is_down && BeginGesture(button, mh)) {
			return 1;
		}
		return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
	}

	// プロジェクトのロード/初期化の直後に呼ばれる。ホストが API を受け付けられる合図にする
	// (フラグを立てるだけで、ホストの API には触らない)
	void OnProjectLoad(PROJECT_FILE*) {
		g_host_ready = true;
	}

} // namespace

// DLL のエントリポイント
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		g_instance = instance;
		DisableThreadLibraryCalls(instance);
	}
	return TRUE;
}

// config2 初期化コールバック
EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* config) {
	g_config = config;
}

// logger2 初期化コールバック
EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* logger) {
	g_logger = logger;
}

EXTERN_C __declspec(dllexport) DWORD RequiredVersion() {
	return kRequiredVersion;
}

// 終了処理　登録したフックなどを解放する
EXTERN_C __declspec(dllexport) void UninitializePlugin() {
	if (g_mouse_hook != nullptr) {
		UnhookWindowsHookEx(g_mouse_hook);
		g_mouse_hook = nullptr;
	}
	ResetGesture();
	g_host_ready = false;
	g_edit_handle = nullptr;
	g_host_window = nullptr;
	g_config = nullptr;
	g_logger = nullptr;
}

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable() {
	return &g_plugin_table;
}

// 登録処理　設定を読み込み、割り当てがあればマウスフックを設置する
EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
	g_edit_handle = host->create_edit_handle();
	if (g_edit_handle == nullptr) {
		Log(LogLevel::Error, L"create_edit_handle に失敗しました。");
		return;
	}

	LoadConfig();

	// ホストの初期化が終わるまで、フックはホストの API を呼ばない (OnProjectLoad が合図)
	g_host_ready = false;
	host->register_project_load_handler(OnProjectLoad);

	// ホストの UI スレッドにだけフックを掛ける (システム全体の入力には影響しない)
	const HWND host_window = g_edit_handle->get_host_app_window();
	g_host_window = host_window;
	DWORD thread_id = (host_window != nullptr) ? GetWindowThreadProcessId(host_window, nullptr) : 0;
	if (thread_id == 0) {
		thread_id = GetCurrentThreadId();
	}

	g_mouse_hook = SetWindowsHookExW(WH_MOUSE, MouseHookProc, g_instance, thread_id);
	if (g_mouse_hook == nullptr) {
		Log(LogLevel::Error, L"SetWindowsHookExW に失敗しました (error=%lu)", GetLastError());
	}
}
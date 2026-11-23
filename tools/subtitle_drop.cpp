#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "subtitle_drop.hpp"
#include "local_subtitles.hpp"

#include <commctrl.h>
#include <ole2.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <atomic>
#include <memory>
#include <new>
#include <utility>

namespace subtitle_drop {
namespace {
using Microsoft::WRL::ComPtr;

// This same-thread OLE window property lets us keep the drop target already
// registered by VLC 3.0.24 and delegate all other drags to it.
constexpr wchar_t ole_target[] = L"OleDropTargetInterface";
constexpr wchar_t installed_target[] = L"VlcRedesign.SubtitleDropTarget";
constexpr UINT_PTR target_subclass = 1;

std::vector<std::wstring> subtitle_paths(IDataObject *data) {
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (!data || FAILED(data->GetData(&format, &medium)))
        return {};
    const std::unique_ptr<STGMEDIUM, decltype(&ReleaseStgMedium)> drop(&medium, ReleaseStgMedium);
    if (medium.tymed != TYMED_HGLOBAL)
        return {};

    const auto files = static_cast<HDROP>(medium.hGlobal);
    const auto count = DragQueryFileW(files, 0xffffffff, nullptr, 0);
    std::vector<std::wstring> paths;
    for (UINT i = 0; i < count; ++i) {
        const auto length = DragQueryFileW(files, i, nullptr, 0);
        if (!length)
            return {};
        std::wstring path(length + 1, L'\0');
        if (DragQueryFileW(files, i, path.data(), length + 1) != length)
            return {};
        path.resize(length);
        if (!local_subtitles::is_srt_file(path))
            return {};
        paths.push_back(std::move(path));
    }
    return paths;
}

class DropTarget final : public IDropTarget {
public:
    DropTarget(IDropTarget *original, CanAttach can_attach, Attach attach)
        : original_(original), can_attach_(can_attach), attach_(attach) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **object) override {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (id != IID_IUnknown && id != IID_IDropTarget)
            return E_NOINTERFACE;
        *object = static_cast<IDropTarget *>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --refs_;
        if (!remaining)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD keys, POINTL point, DWORD *effect) override {
        files_.clear();
        try {
            files_ = subtitle_paths(data);
            if (!files_.empty() && can_attach_()) {
                *effect &= DROPEFFECT_COPY;
                return S_OK;
            }
        } catch (...) {
        }
        files_.clear();
        if (original_)
            return original_->DragEnter(data, keys, point, effect);
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL point, DWORD *effect) override {
        if (!files_.empty())
            *effect &= DROPEFFECT_COPY;
        else if (original_)
            return original_->DragOver(keys, point, effect);
        else
            *effect = DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (!files_.empty())
            files_.clear();
        else if (original_)
            return original_->DragLeave();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD keys, POINTL point, DWORD *effect) override {
        if (files_.empty() && original_)
            return original_->Drop(data, keys, point, effect);
        const auto files = std::exchange(files_, {});
        const bool copy_allowed = (*effect & DROPEFFECT_COPY) != 0;
        *effect = DROPEFFECT_NONE;
        try {
            if (copy_allowed && !files.empty() && attach_(files))
                *effect = DROPEFFECT_COPY;
        } catch (...) {
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    ComPtr<IDropTarget> original_;
    CanAttach can_attach_;
    Attach attach_;
    std::vector<std::wstring> files_;
};

LRESULT CALLBACK detach_target(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
    if (message == WM_NCDESTROY) {
        auto *target = reinterpret_cast<DropTarget *>(data);
        RemoveWindowSubclass(window, detach_target, id);
        RemovePropW(window, installed_target);
        if (GetPropW(window, ole_target) == static_cast<IDropTarget *>(target))
            RevokeDragDrop(window);
        target->Release();
    }
    return DefSubclassProc(window, message, wparam, lparam);
}
}

UINT install_message() {
    static const UINT message = RegisterWindowMessageW(L"VlcRedesign.InstallSubtitleDrop");
    return message;
}

void request(HWND window) {
    if (const UINT message = install_message(); message && !GetPropW(window, installed_target))
        PostMessageW(window, message, 0, 0);
}

void install(HWND window, CanAttach can_attach, Attach attach) {
    if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId() || GetPropW(window, installed_target))
        return;
    const auto original = static_cast<IDropTarget *>(GetPropW(window, ole_target));
    ComPtr<DropTarget> target;
    target.Attach(new (std::nothrow) DropTarget(original, can_attach, attach));
    if (!target)
        return;
    if (!SetPropW(window, installed_target, target.Get()))
        return;
    if (!SetWindowSubclass(window, detach_target, target_subclass, reinterpret_cast<DWORD_PTR>(target.Get()))) {
        RemovePropW(window, installed_target);
        return;
    }
    target->AddRef();  // The window subclass holds this reference until WM_NCDESTROY.

    const bool can_register = !original || SUCCEEDED(RevokeDragDrop(window));
    if (can_register && SUCCEEDED(RegisterDragDrop(window, target.Get())))
        return;
    if (can_register && original)
        RegisterDragDrop(window, original);
    if (RemoveWindowSubclass(window, detach_target, target_subclass))
        target->Release();
    RemovePropW(window, installed_target);
}
}

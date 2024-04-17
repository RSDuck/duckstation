#include "switch_nogui_platform.h"

#include "core/host.h"

#include "util/switch_exception_frame.h"

#include <switch.h>

namespace Common::PageFaultHandler {
bool PageFaultHandler(ExceptionFrameA64* ctx);
}

extern "C" {

extern char __start__;
extern char __rodata_start;

void HandleFault(uint64_t pc, uint64_t lr, uint64_t fp, uint64_t fault_addr, Result desc)
{
  if (pc >= (uint64_t)&__start__ && pc < (uint64_t)&__rodata_start)
  {
    printf("Unintentional fault in .text at %p (type %d) (trying to access %p?)\n", (void*)(pc - (uint64_t)&__start__),
           desc, (void*)fault_addr);

    int frame_num = 0;
    while (true)
    {
      printf("Stack frame %d %p\n", frame_num, (void*)(lr - (uint64_t)&__start__));
      lr = *(uint64_t*)(fp + 8);
      fp = *(uint64_t*)fp;

      frame_num++;
      if (frame_num > 16 || fp == 0 || (fp & 0x7) != 0)
        break;
    }
  }
  else
  {
    printf("Unintentional fault somewhere in deep (address) space at %p (type %d)\n", (void*)pc, desc);
    if (lr >= (uint64_t)&__start__ && lr < (uint64_t)&__rodata_start)
      printf("LR in range: %p\n", (void*)(lr - (uint64_t)&__start__));
  }

  svcBreak(BreakReason_Panic, 0, 0);
}

static_assert(sizeof(ExceptionFrameA64) == 0x78);

alignas(16) uint8_t switch_exception_stack_top[0x8000];

void switch_exception_handler(Result reason, ExceptionFrameA64* frame, u64 fp)
{
  if (Common::PageFaultHandler::PageFaultHandler(frame))
    return;
  HandleFault(frame->pc, frame->lr, fp, frame->far, reason);
}
}

std::unique_ptr<NoGUIPlatform> NoGUIPlatform::CreateSwitchPlatform()
{
  std::unique_ptr<SwitchNoGUIPlatform> platform(std::make_unique<SwitchNoGUIPlatform>());
  if (!platform->Initialize())
    platform.reset();
  return platform;
}

SwitchNoGUIPlatform::SwitchNoGUIPlatform()
{
  m_message_loop_running.store(true, std::memory_order_release);
}

SwitchNoGUIPlatform::~SwitchNoGUIPlatform() = default;

void SwitchNoGUIPlatform::AppletModeChange(AppletHookType type)
{
  switch (type)
  {
    case AppletHookType_OnOperationMode:
    {
      std::optional<WindowInfo> wi = GetPlatformWindowInfo();
      NoGUIHost::ProcessPlatformWindowResize(wi->surface_width, wi->surface_height, wi->surface_scale);
      break;
    }
    default:
      break;
  }
}

static void AppletModeChange(AppletHookType type, void* host_interface)
{
  static_cast<SwitchNoGUIPlatform*>(host_interface)->AppletModeChange(type);
}

bool SwitchNoGUIPlatform::Initialize()
{
  appletHook(&m_applet_cookie, ::AppletModeChange, this);

  return true;
}

void SwitchNoGUIPlatform::ReportError(const std::string_view& title, const std::string_view& message)
{
  // The title is usually just error which is not that informative
  // so we append the first line of the message
  std::string shortError(title);
  shortError += ": ";
  shortError += message.substr(0, message.find('\n'));
  // small hack to make the error messages nicer to look at.
  if (shortError[shortError.size() - 1] == ':')
    shortError[shortError.size() - 1] = '.';
  std::string longError(message);

  ErrorApplicationConfig errapp;
  errorApplicationCreate(&errapp, shortError.c_str(), longError.c_str());

  errorApplicationShow(&errapp);
}

bool SwitchNoGUIPlatform::ConfirmMessage(const std::string_view& title, const std::string_view& message)
{
  return true;
}

void SwitchNoGUIPlatform::SetDefaultConfig(SettingsInterface& si)
{
}

bool SwitchNoGUIPlatform::CreatePlatformWindow(std::string title)
{
  return true;
}

bool SwitchNoGUIPlatform::HasPlatformWindow() const
{
  return true;
}

void SwitchNoGUIPlatform::DestroyPlatformWindow()
{
}

std::optional<WindowInfo> SwitchNoGUIPlatform::GetPlatformWindowInfo()
{
  WindowInfo wi;
  AppletOperationMode mode = appletGetOperationMode();
  if (mode == AppletOperationMode_Handheld)
  {
    wi.surface_width = 1280;
    wi.surface_height = 720;
  }
  else
  {
    wi.surface_width = 1920;
    wi.surface_height = 1080;
  }
  wi.surface_scale = 1.2f;
  wi.surface_refresh_rate = 60.f;
  wi.surface_format = GPUTexture::Format::RGBA8;
  wi.type = WindowInfo::Type::Switch;
  wi.window_handle = nwindowGetDefault();
  return wi;
}

void SwitchNoGUIPlatform::SetPlatformWindowTitle(std::string title)
{
}

std::optional<u32> SwitchNoGUIPlatform::ConvertHostKeyboardStringToCode(const std::string_view& str)
{
  return std::nullopt;
}

std::optional<std::string> SwitchNoGUIPlatform::ConvertHostKeyboardCodeToString(u32 code)
{
  return std::nullopt;
}

void SwitchNoGUIPlatform::RunMessageLoop()
{
  while (m_message_loop_running.load(std::memory_order_acquire))
  {
    if (!appletMainLoop())
      NoGUIHost::StopRunning();

    std::unique_lock lock(m_callback_queue_mutex);
    while (!m_callback_queue.empty())
    {
      std::function<void()> func = std::move(m_callback_queue.front());
      m_callback_queue.pop_front();
      lock.unlock();
      func();
      lock.lock();
    }
  }
}

void SwitchNoGUIPlatform::ExecuteInMessageLoop(std::function<void()> func)
{
  std::unique_lock lock(m_callback_queue_mutex);
  m_callback_queue.push_back(std::move(func));
}

void SwitchNoGUIPlatform::QuitMessageLoop()
{
  m_message_loop_running.store(false, std::memory_order_release);
}

void SwitchNoGUIPlatform::SetFullscreen(bool enabled)
{
}

bool SwitchNoGUIPlatform::RequestRenderWindowSize(s32 new_window_width, s32 new_window_height)
{
  return false;
}

bool SwitchNoGUIPlatform::OpenURL(const std::string_view& url)
{
  return false;
}

bool SwitchNoGUIPlatform::CopyTextToClipboard(const std::string_view& text)
{
  return false;
}

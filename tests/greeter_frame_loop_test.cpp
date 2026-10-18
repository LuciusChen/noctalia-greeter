#include "greeter/greeter_surface.h"
#include "greeter/greeter_window.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/text/glyph_registry.h"
#include "wayland/wayland_client.h"

#include <EGL/egl.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <poll.h>
#include <stdexcept>
#include <wayland-client.h>

namespace {

  using namespace std::chrono_literals;

  std::size_t swaps = 0;
  bool frameCallbackPending = false;
  bool swappedWithoutCallback = false;

  // Observe the real window callback without replacing its frame handling.
  const wl_callback_listener kFrameListener = {
      .done = [](void* data, wl_callback* callback, std::uint32_t time) {
        frameCallbackPending = false;
        GreeterWindow::handleFrameDone(data, callback, time);
      },
  };

  void require(bool condition, const char* message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  bool dispatchUntil(WaylandClient& client, std::chrono::milliseconds timeout, const std::function<bool()>& done) {
    wl_display* display = client.display();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      require(wl_display_dispatch_pending(display) >= 0, "dispatch_pending failed");
      require(!swappedWithoutCallback, "a frame callback was requested after the EGL swap/commit");
      if (done()) {
        return true;
      }
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (remaining <= 0ms) {
        return false;
      }
      if (wl_display_prepare_read(display) != 0) {
        continue;
      }
      if (client.flush() < 0) {
        wl_display_cancel_read(display);
        throw std::runtime_error("Wayland flush failed");
      }
      pollfd fd{.fd = wl_display_get_fd(display), .events = POLLIN, .revents = 0};
      const int result = poll(&fd, 1, static_cast<int>(remaining.count()));
      if (result > 0 && (fd.revents & POLLIN) != 0) {
        require(wl_display_read_events(display) >= 0, "Wayland read failed");
      } else {
        wl_display_cancel_read(display);
        require(result >= 0 || errno == EINTR, "Wayland poll failed");
        require((fd.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0, "Wayland connection closed");
      }
    }
  }

  void dispatchFor(WaylandClient& client, std::chrono::milliseconds duration) {
    (void)dispatchUntil(client, duration, []() { return false; });
  }

  void requireIdle(WaylandClient& client, GreeterWindow& window) {
    // Drain the callback for the last painted frame before checking for idle.
    dispatchFor(client, 100ms);
    const auto before = swaps;
    window.renderNow();
    dispatchFor(client, 100ms);
    require(swaps == before, "an idle window kept swapping buffers");
  }

  void testFrameLoop() {
    WaylandClient client;
    require(client.connect(), "could not connect to the headless compositor");
    require(
        dispatchUntil(client, 5s, [&]() { return !client.readyOutputsSorted().empty(); }),
        "headless output was not announced"
    );
    GlSharedContext gl;
    gl.initialize(client.display());
    RenderContext context;
    context.initialize(gl);
    GlyphRegistry::initialize();
    GreeterSurface surface;
    surface.initialize(&context);
    GreeterWindow window(client, gl, context, surface);
    surface.setWindow(&window);
    require(window.createSurface(), "could not create the test window");
    const auto outputs = client.readyOutputsSorted();
    window.bindOutput(outputs.front()->output);

    auto* animations = surface.sceneRoot()->animationManager();
    require(animations != nullptr, "scene has no animation manager");
    int owner = 0;
    unsigned updates = 0;
    // Deliberately leave the scene clean in onUpdate: active animations alone
    // must keep prepareFrame() and the callback/commit cycle running.
    animations->animate(0.0f, 1.0f, 60000.0f, Easing::Linear, [&](float) { ++updates; }, nullptr, &owner);
    window.setSceneReady(true);
    require(
        dispatchUntil(client, 5s, [&]() { return updates >= 4; }),
        "animation stalled without input or an unrelated commit"
    );
    animations->cancelForOwner(&owner);
    require(!surface.animating(), "cancelled animation is still active");
    requireIdle(client, window);

    auto before = swaps;
    window.requestRedraw();
    require(swaps == before + 1, "explicit redraw did not paint");
    requireIdle(client, window);

    before = swaps;
    window.requestLayout();
    require(swaps == before + 1, "explicit layout did not paint");
    requireIdle(client, window);

    before = swaps;
    surface.sceneRoot()->markPaintDirty();
    window.renderNow();
    require(swaps == before + 1, "dirty scene paint did not paint");
    requireIdle(client, window);

    before = swaps;
    surface.sceneRoot()->markLayoutDirty();
    window.renderNow();
    require(swaps == before + 1, "dirty scene layout did not paint");
    requireIdle(client, window);

    // Wake from idle, finish naturally, and stop committing again.
    window.requestRedraw();
    updates = 0;
    animations->animate(0.0f, 1.0f, 200.0f, Easing::Linear, [&](float) { ++updates; }, nullptr, &owner);
    window.requestRedraw();
    require(dispatchUntil(client, 5s, [&]() { return !surface.animating(); }), "restarted animation did not finish");
    require(updates >= 2, "restarted animation did not advance over multiple frames");
    requireIdle(client, window);
    std::puts("frame loop: startup, animation, redraw/layout, idle, and restart passed");
  }

} // namespace

// Linker wrappers observe production requests and swaps, forwarding both to
// libwayland/EGL. In particular, these catch ordering regressions in either
// renderNow() or handleFrameDone(), even if a later configure masks the stall.
extern "C" int __real_wl_proxy_add_listener(wl_proxy* proxy, void (**implementation)(void), void* data);
extern "C" int __wrap_wl_proxy_add_listener(wl_proxy* proxy, void (**implementation)(void), void* data) {
  if (std::strcmp(wl_proxy_get_class(proxy), "wl_callback") == 0) {
    const auto* listener = reinterpret_cast<const wl_callback_listener*>(implementation);
    if (listener->done == &GreeterWindow::handleFrameDone) {
      frameCallbackPending = true;
      return wl_callback_add_listener(reinterpret_cast<wl_callback*>(proxy), &kFrameListener, data);
    }
  }
  return __real_wl_proxy_add_listener(proxy, implementation, data);
}

extern "C" EGLBoolean __real_eglSwapBuffers(EGLDisplay display, EGLSurface surface);
extern "C" EGLBoolean __wrap_eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
  swappedWithoutCallback = swappedWithoutCallback || !frameCallbackPending;
  ++swaps;
  return __real_eglSwapBuffers(display, surface);
}

int main() {
  try {
    testFrameLoop();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "frame loop: %s\n", error.what());
    return 1;
  }
}

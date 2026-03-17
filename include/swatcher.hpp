/*
 * swatcher.hpp — C++17 header-only wrapper for swatcher
 *
 * Usage:
 *   sw::Watcher w;
 *   w.add(sw::Target("./mydir", SWATCHER_EVENT_ALL)
 *       .recursive()
 *       .callback_patterns({"*.cpp", "*.h"})
 *       .on_event([](swatcher_fs_event ev, const char *name, void *) {
 *           std::cout << swatcher_event_name(ev) << ": " << name << "\n";
 *       }));
 *   w.start();
 */

#ifndef SWATCHER_HPP
#define SWATCHER_HPP

#include "swatcher.h"

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sw {

/* ---------- Callback wrapper ---------- */

struct CallbackData {
    std::function<void(swatcher_fs_event, const char *, void *)> fn;
    void *user_data;
};

inline void trampoline(swatcher_fs_event event, swatcher_target *target,
                       const char *name, void *data)
{
    (void)data;
    auto *cbd = static_cast<CallbackData *>(target->user_data);
    if (cbd && cbd->fn)
        cbd->fn(event, name, cbd->user_data);
}

/* ---------- Target ---------- */

class Target {
    swatcher_target *handle_ = nullptr;
    std::vector<std::string> cb_patterns_, watch_patterns_, ignore_patterns_;
    std::vector<const char *> cb_ptrs_, watch_ptrs_, ignore_ptrs_;
    CallbackData *cbd_ = nullptr;

    swatcher_target_desc desc_{};
    bool built_ = false;

    static void build_ptrs(const std::vector<std::string> &src,
                           std::vector<const char *> &dst)
    {
        dst.clear();
        for (auto &s : src)
            dst.push_back(s.c_str());
        dst.push_back(nullptr);
    }

public:
    Target(std::string_view path, swatcher_fs_event events,
           swatcher_watch_option opts = SWATCHER_WATCH_ALL)
    {
        /* Store path string in cb_patterns_[0] to keep it alive */
        cb_patterns_.push_back(std::string(path));
        desc_.path = const_cast<char *>(cb_patterns_[0].c_str());
        desc_.events = events;
        desc_.watch_options = opts;
        desc_.is_recursive = false;
        desc_.follow_symlinks = false;
    }

    ~Target()
    {
        if (handle_ && !built_)
            swatcher_target_destroy(handle_);
        delete cbd_;
    }

    Target(Target &&o) noexcept
        : handle_(o.handle_), cb_patterns_(std::move(o.cb_patterns_)),
          watch_patterns_(std::move(o.watch_patterns_)),
          ignore_patterns_(std::move(o.ignore_patterns_)),
          cb_ptrs_(std::move(o.cb_ptrs_)),
          watch_ptrs_(std::move(o.watch_ptrs_)),
          ignore_ptrs_(std::move(o.ignore_ptrs_)),
          cbd_(o.cbd_), desc_(o.desc_), built_(o.built_)
    {
        o.handle_ = nullptr;
        o.cbd_ = nullptr;
    }

    Target &operator=(Target &&o) noexcept
    {
        if (this != &o) {
            if (handle_ && !built_) swatcher_target_destroy(handle_);
            delete cbd_;
            handle_ = o.handle_;
            cb_patterns_ = std::move(o.cb_patterns_);
            watch_patterns_ = std::move(o.watch_patterns_);
            ignore_patterns_ = std::move(o.ignore_patterns_);
            cb_ptrs_ = std::move(o.cb_ptrs_);
            watch_ptrs_ = std::move(o.watch_ptrs_);
            ignore_ptrs_ = std::move(o.ignore_ptrs_);
            cbd_ = o.cbd_;
            desc_ = o.desc_;
            built_ = o.built_;
            o.handle_ = nullptr;
            o.cbd_ = nullptr;
        }
        return *this;
    }

    Target(const Target &) = delete;
    Target &operator=(const Target &) = delete;

    Target &recursive(bool r = true)
    {
        desc_.is_recursive = r;
        return *this;
    }

    Target &follow_symlinks(bool f = true)
    {
        desc_.follow_symlinks = f;
        return *this;
    }

    Target &callback_patterns(std::vector<std::string> patterns)
    {
        /* cb_patterns_[0] is the path, keep it */
        std::string saved_path;
        if (!cb_patterns_.empty())
            saved_path = cb_patterns_[0];
        cb_patterns_ = std::move(patterns);
        if (!saved_path.empty())
            cb_patterns_.insert(cb_patterns_.begin(), saved_path);
        return *this;
    }

    Target &watch_patterns_set(std::vector<std::string> patterns)
    {
        watch_patterns_ = std::move(patterns);
        return *this;
    }

    Target &ignore_patterns_set(std::vector<std::string> patterns)
    {
        ignore_patterns_ = std::move(patterns);
        return *this;
    }

    Target &on_event(std::function<void(swatcher_fs_event, const char *, void *)> fn,
                     void *user_data = nullptr)
    {
        delete cbd_;
        cbd_ = new CallbackData{std::move(fn), user_data};
        return *this;
    }

    /* Build the C target. Called by Watcher::add. */
    swatcher_target *build()
    {
        if (handle_) return handle_;

        /* Path is stored in cb_patterns_[0] */
        desc_.path = const_cast<char *>(cb_patterns_[0].c_str());

        /* Build callback pattern pointers (skip [0] which is the path) */
        if (cb_patterns_.size() > 1) {
            cb_ptrs_.clear();
            for (size_t i = 1; i < cb_patterns_.size(); i++)
                cb_ptrs_.push_back(cb_patterns_[i].c_str());
            cb_ptrs_.push_back(nullptr);
            desc_.callback_patterns = const_cast<char **>(cb_ptrs_.data());
        }

        if (!watch_patterns_.empty()) {
            build_ptrs(watch_patterns_, watch_ptrs_);
            desc_.watch_patterns = const_cast<char **>(watch_ptrs_.data());
        }

        if (!ignore_patterns_.empty()) {
            build_ptrs(ignore_patterns_, ignore_ptrs_);
            desc_.ignore_patterns = const_cast<char **>(ignore_ptrs_.data());
        }

        if (cbd_) {
            desc_.callback = trampoline;
            desc_.user_data = cbd_;
        }

        handle_ = swatcher_target_create(&desc_);
        return handle_;
    }

    swatcher_target *release()
    {
        built_ = true;
        return handle_;
    }

    swatcher_target *get() const { return handle_; }
};

/* ---------- Watcher ---------- */

class Watcher {
    swatcher *sw_ = nullptr;
    swatcher_config config_{};
    std::vector<Target> targets_;

public:
    explicit Watcher(swatcher_config config = {}, const char *backend = nullptr)
        : config_(config)
    {
        sw_ = static_cast<swatcher *>(malloc(sizeof(swatcher)));
        if (!sw_) return;

        bool ok;
        if (backend)
            ok = swatcher_init_with_backend(sw_, &config_, backend);
        else
            ok = swatcher_init(sw_, &config_);

        if (!ok) {
            free(sw_);
            sw_ = nullptr;
        }
    }

    ~Watcher()
    {
        if (sw_) {
            swatcher_stop(sw_);
            swatcher_cleanup(sw_);
            free(sw_);
        }
    }

    Watcher(const Watcher &) = delete;
    Watcher &operator=(const Watcher &) = delete;

    Watcher(Watcher &&o) noexcept
        : sw_(o.sw_), config_(o.config_), targets_(std::move(o.targets_))
    {
        o.sw_ = nullptr;
    }

    explicit operator bool() const { return sw_ != nullptr; }

    bool add(Target &&target)
    {
        if (!sw_) return false;
        swatcher_target *t = target.build();
        if (!t) return false;
        target.release();
        bool ok = swatcher_add(sw_, t);
        if (ok)
            targets_.push_back(std::move(target));
        return ok;
    }

    bool start()
    {
        return sw_ ? swatcher_start(sw_) : false;
    }

    void stop()
    {
        if (sw_) swatcher_stop(sw_);
    }

    bool is_watched(const char *path) const
    {
        return sw_ ? swatcher_is_watched(sw_, path) : false;
    }

    static swatcher_error last_error()
    {
        return swatcher_last_error();
    }

    static const char *last_error_string()
    {
        return swatcher_error_string(swatcher_last_error());
    }
};

} /* namespace sw */

#endif /* SWATCHER_HPP */

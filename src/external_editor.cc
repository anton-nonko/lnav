/**
 * Copyright (c) 2024, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <optional>
#include <thread>

#include "external_editor.hh"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "base/auto_fd.hh"
#include "base/auto_pid.hh"
#include "base/fs_util.hh"
#include "base/injector.hh"
#include "base/lnav_log.hh"
#include "base/result.h"
#include "external_editor.cfg.hh"
#include "fmt/format.h"
#include "shlex.hh"

namespace lnav::external_editor {

static std::optional<impl>
get_impl()
{
    const auto& cfg = injector::get<const config&>();

    log_debug("editor impl count: %d", cfg.c_impls.size());
    for (const auto& pair : cfg.c_impls) {
        const auto full_cmd = fmt::format(FMT_STRING("{} > /dev/null 2>&1"),
                                          pair.second.i_test_command);

        log_debug("testing editor impl %s using: %s",
                  pair.first.c_str(),
                  full_cmd.c_str());
        if (system(full_cmd.c_str()) == 0) {
            log_info("detected editor: %s", pair.first.c_str());
            return pair.second;
        }
    }
    if (cfg.c_impls.empty()) {
        log_error("no external editor implementations given!");
    }

    return std::nullopt;
}

Result<void, std::string>
open(std::filesystem::path p)
{
    static const auto IMPL = get_impl();

    if (!IMPL) {
        const static std::string MSG = "no external editor found";

        return Err(MSG);
    }

    log_info("external editor command: %s", IMPL->i_command.c_str());

    auto err_pipe = TRY(auto_pipe::for_child_fd(STDERR_FILENO));
    auto child_pid_res = lnav::pid::from_fork();
    if (child_pid_res.isErr()) {
        return Err(child_pid_res.unwrapErr());
    }

    auto child_pid = child_pid_res.unwrap();
    err_pipe.after_fork(child_pid.in());
    if (child_pid.in_child()) {
        auto open_res
            = lnav::filesystem::open_file("/dev/null", O_RDONLY | O_CLOEXEC);
        open_res.then([](auto_fd&& fd) {
            fd.copy_to(STDIN_FILENO);
            fd.copy_to(STDOUT_FILENO);
        });
        setenv("FILE_PATH", p.c_str(), 1);

        execlp("sh", "sh", "-c", IMPL->i_command.c_str(), nullptr);
        _exit(EXIT_FAILURE);
    }
    log_debug("started external editor, pid: %d", child_pid.in());

    std::string error_queue;
    std::thread err_reader(
        [err = std::move(err_pipe.read_end()), &error_queue]() mutable {
            while (true) {
                char buffer[1024];
                auto rc = read(err.get(), buffer, sizeof(buffer));
                if (rc <= 0) {
                    break;
                }

                error_queue.append(buffer, rc);
            }

            log_debug("external editor stderr closed");
        });

    auto finished_child = std::move(child_pid).wait_for_child();
    err_reader.join();
    if (!finished_child.was_normal_exit()) {
        return Err(fmt::format(FMT_STRING("editor failed with signal {}"),
                               finished_child.term_signal()));
    }
    auto exit_status = finished_child.exit_status();
    if (exit_status != 0) {
        return Err(fmt::format(FMT_STRING("editor failed with status {} -- {}"),
                               exit_status,
                               error_queue));
    }

    return Ok();
}

}  // namespace lnav::external_editor

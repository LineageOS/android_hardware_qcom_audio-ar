/*
 * SPDX-FileCopyrightText: 2025 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "AHAL: UltrasoundProximity"

#include "AudioCommon.h"
#include "AudioDevice.h"
#include "PalApi.h"
#include "kvh2xml.h"
#include "us_detect_api.h"

#include <cutils/sockets.h>
#include <dlfcn.h>
#include <sys/epoll.h>

#define MAX_CLIENTS 3
#define MAX_EVENTS 5
#define MESSAGE_LENGTH 72

constexpr static const char* kUltrasoundNotifyLibrary = "libultrasound_notify.so";
static void* notify_ultrasound_lib_handle = nullptr;
static void (*ultraSoundEventPut)(int event) = nullptr;

static pthread_t g_audio_hal_con;
static bool is_thread_running = false;
static int control_socket_fd, epoll_fd = -1;
static int pipe_fds[2] = {-1, -1};

static pal_stream_handle_t* pal_stream;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t async_start_thread;
static pthread_t delay_stop_thread;
static int upd_state_count = 0;

static void* ConThreadProcess(void* param) {
    int client_fds[MAX_CLIENTS] = {-1, -1, -1};
    int client_count = 0;
    struct epoll_event events[MAX_EVENTS];
    int num_events, client_fd, bytes_read;
    char buffer[MESSAGE_LENGTH];

    while (true) {
        num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        if (num_events == -1) {
            if (errno == EINTR) continue;
            AHAL_INFO("epoll_wait failed (errno=%d %s)", errno, strerror(errno));
            AHAL_INFO("ConThreadProcess() exit");
            return NULL;
        }

        for (int i = 0; i < num_events; i++) {
            int event_fd = events[i].data.fd;
            uint32_t event_flags = events[i].events;

            // new client connections
            if (event_fd == control_socket_fd) {
                if (event_flags & EPOLLIN) {
                    client_fd = accept(control_socket_fd, NULL, NULL);
                    if (client_fd < 0) {
                        AHAL_INFO("accept failed %d", client_fd);
                    } else if (client_count < MAX_CLIENTS) {
                        AHAL_INFO("Accepted client %d, total connections %d", client_fd,
                                  client_count + 1);

                        client_fds[client_count++] = client_fd;

                        struct epoll_event client_event;
                        client_event.events = EPOLLIN | EPOLLRDHUP;
                        client_event.data.fd = client_fd;
                        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_event);
                    } else {
                        AHAL_INFO("Client connections full");
                        close(client_fd);
                    }
                }
            }
            // pipe messages (exit signal)
            else if (event_fd == epoll_fd) {
                if (event_flags & EPOLLIN) {
                    read(event_fd, buffer, 1);
                    if (buffer[0] == '\0') {
                        AHAL_INFO("thread exit");
                        pthread_exit(NULL);
                    }
                }
            }
            // client messages
            else {
                if (event_flags & EPOLLIN) {
                    bytes_read = read(event_fd, buffer, sizeof(buffer));

                    if (bytes_read == 0) {
                        AHAL_INFO("EOF on control data socket");
                    } else if (bytes_read < 0) {
                        AHAL_INFO("control data socket read failed; errno=%d", errno);
                    } else if (bytes_read != MESSAGE_LENGTH) {
                        AHAL_INFO("message too short %d", bytes_read);
                    } else {
                        AHAL_INFO("Received message from client %d", event_fd);

                        if (buffer[0] == '*') {
                            int cmd = buffer[8];
                            AHAL_INFO("Received ULTRASOUND_ENABLE_CMD, payload[%d]", cmd);
                            std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
                            if (!adevice) {
                                AHAL_ERR("invalid adevice object");
                            }
                            if (cmd == 0) {
                                adevice->SetParameters("ultrasound-proximity=0");
                            } else if (cmd == 1) {
                                adevice->SetParameters("ultrasound-proximity=1");
                            } else {
                                AHAL_INFO("Unknown payload %d", cmd);
                            }
                        } else {
                            AHAL_INFO("Unknown command code");
                        }
                        buffer[0] = 1;
                        write(event_fd, buffer, 2);
                    }
                }
                if (event_flags & (EPOLLHUP | EPOLLERR)) {
                    AHAL_INFO("EPOLLHUP on client socket %d", event_fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, event_fd, NULL);
                    close(event_fd);
                    for (int j = 0; j < client_count; j++) {
                        if (client_fds[j] == event_fd) {
                            client_fds[j] = -1;
                            break;
                        }
                    }
                    client_count--;
                }
            }
        }
    }
}

void audio_hal_con_thread_start() {
    notify_ultrasound_lib_handle = dlopen(kUltrasoundNotifyLibrary, RTLD_NOW);
    if (!notify_ultrasound_lib_handle) {
        AHAL_ERR("Failed to load %s: %s", kUltrasoundNotifyLibrary, dlerror());
        return;
    }

    ultraSoundEventPut = (void (*)(int))dlsym(notify_ultrasound_lib_handle, "ultraSoundEventPut");
    if (!ultraSoundEventPut) {
        AHAL_ERR("Failed to load ultraSoundEventPut from %s: %s", kUltrasoundNotifyLibrary,
                 dlerror());
        dlclose(notify_ultrasound_lib_handle);
        notify_ultrasound_lib_handle = nullptr;
        return;
    }

    if (pipe(pipe_fds) < 0) {
        AHAL_ERR("%s() pipe failed (%s)", __func__, strerror(errno));
        return;
    }

    control_socket_fd = android_get_control_socket("audio_us_socket_0");
    if (control_socket_fd < 0) {
        AHAL_ERR("%s() Failed to get control socket", __func__);
        return;
    }

    if (listen(control_socket_fd, 1) < 0) {
        AHAL_ERR("%s() listen error %s", __func__, strerror(errno));
        return;
    }

    epoll_fd = epoll_create(5);
    if (epoll_fd < 0) {
        AHAL_ERR("%s() epoll_create failed %s", __func__, strerror(errno));
        return;
    }

    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = control_socket_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, control_socket_fd, &event) < 0) {
        AHAL_ERR("%s() epoll_ctl failed %s", __func__, strerror(errno));
        return;
    }

    event.data.fd = pipe_fds[0];
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pipe_fds[0], &event) < 0) {
        AHAL_ERR("%s() epoll_ctl failed %s", __func__, strerror(errno));
        return;
    }

    if (pthread_create(&g_audio_hal_con, nullptr, ConThreadProcess, &g_audio_hal_con) != 0) {
        AHAL_ERR("%s() pthread_create failed %s", __func__, strerror(errno));
        return;
    }

    is_thread_running = true;
    AHAL_INFO("Started ultrasound proximity connection thread");
}

int audio_hal_con_thread_exit() {
    if (is_thread_running) {
        char exit_signal = 0;

        write(pipe_fds[1], &exit_signal, 1);
        pthread_join(g_audio_hal_con, nullptr);

        if (control_socket_fd != -1) {
            close(control_socket_fd);
            control_socket_fd = -1;
        }

        if (pipe_fds[0] != -1) {
            close(pipe_fds[0]);
            pipe_fds[0] = -1;
        }

        if (pipe_fds[1] != -1) {
            close(pipe_fds[1]);
            pipe_fds[1] = -1;
        }

        if (epoll_fd != -1) {
            close(epoll_fd);
            epoll_fd = -1;
        }

        is_thread_running = false;
    }

    if (notify_ultrasound_lib_handle) {
        dlclose(notify_ultrasound_lib_handle);
        notify_ultrasound_lib_handle = nullptr;
        ultraSoundEventPut = nullptr;
    }

    AHAL_INFO("Stopped ultrasound proximity connection thread");

    return 0;
}

static int32_t HandleCallbackForUPD(pal_stream_handle_t* stream_handle, uint32_t event_id,
                                    uint32_t* event_data, uint32_t event_size, uint64_t cookie) {
    int32_t status = 0;

    if (event_id == EVENT_ID_GENERIC_US_DETECTION) {
        switch (*event_data) {
            case US_DETECT_NEAR:
                AHAL_INFO("Event Detected : Near event received");
                ultraSoundEventPut(0);
                break;
            case US_DETECT_FAR:
                AHAL_INFO("Event Detected : Far event received");
                ultraSoundEventPut(1);
                break;
            default:
                AHAL_INFO("Event Detected : Invalid event %d", *event_data);
                break;
        }
    }
    return status;
}

void* AsyncStartThreadLoop(void* param) {
    pal_param_payload* param_payload = NULL;
    pal_param_upd_event_detection_t payload;
    uint32_t no_of_devices = 2;
    struct pal_device devices[2] = {};
    struct pal_stream_attributes stream_attributes = {};
    int32_t status = 0;

    pthread_detach(pthread_self());
    pthread_mutex_lock(&lock);

    if (upd_state_count > 0) {
        upd_state_count++;
        goto exit;
    }

    // setting stream attributes
    stream_attributes.type = PAL_STREAM_ULTRASOUND;
    stream_attributes.direction = PAL_AUDIO_INPUT_OUTPUT;

    // setting device attriutes
    //  device attributes for UPD will be set based on BE used.
    devices[0].id = PAL_DEVICE_OUT_ULTRASOUND;
    devices[0].config.sample_rate = 96000;

    devices[1].id = PAL_DEVICE_IN_ULTRASOUND_MIC;
    devices[1].config.sample_rate = 96000;

    status = pal_stream_open(&stream_attributes, no_of_devices, devices, 0, NULL,
                             (pal_stream_callback)&HandleCallbackForUPD, 0, &pal_stream);
    if (status) {
        AHAL_ERR("Failed to open UPD stream");
        goto exit;
    }

    param_payload = (pal_param_payload*)calloc(
            1, sizeof(pal_param_payload) + sizeof(pal_param_upd_event_detection_t));
    if (!param_payload) goto close_stream;

    param_payload->payload_size = sizeof(pal_param_upd_event_detection_t);
    payload.register_status = 1;
    memcpy(param_payload->payload, &payload, param_payload->payload_size);

    status = pal_stream_set_param(pal_stream, PAL_PARAM_ID_UPD_REGISTER_FOR_EVENTS, param_payload);
    free(param_payload);
    if (status) {
        AHAL_ERR("Failed to register for UPD events");
        goto close_stream;
    }

    status = pal_stream_start(pal_stream);
    if (status) {
        AHAL_ERR("Failed to start UPD");
        goto close_stream;
    }

    upd_state_count++;

exit:
    pthread_mutex_unlock(&lock);
    pthread_exit(nullptr);

    return NULL;

close_stream:
    pal_stream_close(pal_stream);
    pal_stream = nullptr;
    goto exit;
}

static void* DelayStopThreadLoop(void* param) {
    pal_param_payload* param_payload = NULL;
    pal_param_upd_notify payload;
    int stop_latency;
    int32_t status = 0;

    pthread_detach(pthread_self());

    int delay_ms = property_get_int32("vendor.audio.ultrasound.usync", 1000);
    usleep(delay_ms * 1000);

    pthread_mutex_lock(&lock);
    if (upd_state_count > 1) {
        upd_state_count--;
        goto exit;
    }

    // Send ramp-down event
    param_payload =
            (pal_param_payload*)calloc(1, sizeof(pal_param_payload) + sizeof(pal_param_upd_notify));
    if (!param_payload) goto exit;

    param_payload->payload_size = sizeof(pal_param_upd_notify);
    payload.msg = PCM_DEEP_BUFFER;
    memcpy(param_payload->payload, &payload, param_payload->payload_size);

    status = pal_stream_set_param(pal_stream, PAL_PARAM_ID_UPD_NOTIFY_MSG, param_payload);
    free(param_payload);
    if (status) {
        AHAL_ERR("Failed to start rampdown");
        goto stop_stream;
    }

    stop_latency = property_get_int32("vendor.audio.ultrasound.stoplatency", 75);
    usleep(stop_latency * 1000);

stop_stream:
    if (pal_stream) {
        status = pal_stream_stop(pal_stream);
        if (status) {
            AHAL_ERR("pal_stream_stop failed");
        }

        status = pal_stream_close(pal_stream);
        if (status) {
            AHAL_ERR("pal_stream_close failed");
        }
        pal_stream = nullptr;
    }

    upd_state_count--;

exit:
    pthread_mutex_unlock(&lock);
    pthread_exit(nullptr);

    return NULL;
}

int ultrasound_extn_enable(bool enable) {
    int result;
    if (enable) {
        result = pthread_create(&async_start_thread, nullptr, AsyncStartThreadLoop, nullptr);
        if (result) AHAL_ERR("_ultrasound_start_async() failed %s", strerror(errno));
    } else {
        result = pthread_create(&delay_stop_thread, nullptr, DelayStopThreadLoop, nullptr);
        if (result) AHAL_ERR("_ultrasound_stop_async() failed %s", strerror(errno));
    }

    return result;
}

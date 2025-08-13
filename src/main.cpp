#include <uwebsockets/App.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <thread>
#include <mutex>
#include <vector>
#include <queue>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

// --- Global Shared Data & Structures ---
std::mutex g_media_mutex;
std::vector<uint8_t> g_init_segment;
std::queue<std::vector<uint8_t>> g_media_segments;
bool g_is_init_segment_ready = false;

// --- FFmpeg Custom I/O ---
static int write_packet_callback(void* opaque, uint8_t* buf, int buf_size) {
    std::lock_guard<std::mutex> lock(g_media_mutex);

    if (!g_is_init_segment_ready) {
        g_init_segment.insert(g_init_segment.end(), buf, buf + buf_size);
    } else {
        g_media_segments.push(std::vector<uint8_t>(buf, buf + buf_size));
    }
    return buf_size;
}

// --- FFmpeg Thread ---
void rtsp_thread_function() {
    const char* rtsp_url = "rtsp://wowzaec2demo.streamlock.net/vod/mp4:BigBuckBunny_115k.mov";
    AVFormatContext* ifmt_ctx = nullptr;
    AVFormatContext* ofmt_ctx = nullptr;
    AVIOContext* avio_ctx = nullptr;
    uint8_t* avio_ctx_buffer = nullptr;
    size_t avio_ctx_buffer_size = 8192; // Increased buffer size
    int video_stream_index = -1;

    avformat_network_init();

    if (avformat_open_input(&ifmt_ctx, rtsp_url, nullptr, nullptr) < 0) {
        std::cerr << "FFMPEG: Could not open input: " << rtsp_url << std::endl;
        return;
    }

    if (avformat_find_stream_info(ifmt_ctx, nullptr) < 0) {
        std::cerr << "FFMPEG: Could not find stream information" << std::endl;
        goto end;
    }

    video_stream_index = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index < 0) {
        std::cerr << "FFMPEG: Could not find a video stream" << std::endl;
        goto end;
    }

    avformat_alloc_output_context2(&ofmt_ctx, nullptr, "mp4", nullptr);
    if (!ofmt_ctx) {
        std::cerr << "FFMPEG: Could not create output context" << std::endl;
        goto end;
    }

    avio_ctx_buffer = (uint8_t*)av_malloc(avio_ctx_buffer_size);
    avio_ctx = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size, 1, nullptr, nullptr, &write_packet_callback, nullptr);
    ofmt_ctx->pb = avio_ctx;

    AVStream* out_stream = avformat_new_stream(ofmt_ctx, nullptr);
    avcodec_parameters_copy(out_stream->codecpar, ifmt_ctx->streams[video_stream_index]->codecpar);
    out_stream->codecpar->codec_tag = 0;

    av_opt_set(ofmt_ctx->priv_data, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);

    if (avformat_write_header(ofmt_ctx, nullptr) < 0) {
        std::cerr << "FFMPEG: Error occurred when opening output" << std::endl;
        goto end;
    }

    {
        std::lock_guard<std::mutex> lock(g_media_mutex);
        g_is_init_segment_ready = true;
        std::cout << "FFMPEG: Initialization segment is ready (" << g_init_segment.size() << " bytes)" << std::endl;
    }

    AVPacket pkt;
    while (av_read_frame(ifmt_ctx, &pkt) >= 0) {
        if (pkt.stream_index == video_stream_index) {
            pkt.stream_index = out_stream->index;
            av_packet_rescale_ts(&pkt, ifmt_ctx->streams[video_stream_index]->time_base, out_stream->time_base);

            if (av_interleaved_write_frame(ofmt_ctx, &pkt) < 0) {
                std::cerr << "FFMPEG: Error while writing frame" << std::endl;
                break;
            }
        }
        av_packet_unref(&pkt);
    }

    av_write_trailer(ofmt_ctx);

end:
    if (ifmt_ctx) avformat_close_input(&ifmt_ctx);
    if (ofmt_ctx) {
        if (avio_ctx) {
            av_freep(&avio_ctx->buffer);
            av_freep(&avio_ctx);
        }
        avformat_free_context(ofmt_ctx);
    }
    avformat_network_deinit();
    std::cout << "FFMPEG: Thread finished." << std::endl;
}

// --- uWebSockets Server ---
std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string getMimeType(const std::string& path) {
    if (path.ends_with(".html")) return "text/html; charset=utf-8";
    if (path.ends_with(".js")) return "application/javascript; charset=utf-8";
    return "application/octet-stream";
}

int main() {
    std::thread(rtsp_thread_function).detach();

    const int port = 8080;
    const std::string public_dir = "public";
    struct PerSocketData {};

    uWS::App app;

    app.ws<PerSocketData>("/stream", {
        .compression = uWS::SHARED_COMPRESSOR,
        .maxPayloadLength = 16 * 1024 * 1024,
        .idleTimeout = 30,
        .open = [](auto *ws) {
            std::cout << "WS: Client connected. Subscribing to broadcast." << std::endl;
            ws->subscribe("broadcast");
        },
        .message = [](auto *ws, std::string_view message, uWS::OpCode opCode) {
            if (message == "get_init") {
                std::cout << "WS: Received init request from client." << std::endl;
                std::lock_guard<std::mutex> lock(g_media_mutex);
                if (g_is_init_segment_ready && !g_init_segment.empty()) {
                    ws->send(std::string_view((char*)g_init_segment.data(), g_init_segment.size()), uWS::BINARY);
                    std::cout << "WS: Sent init segment (" << g_init_segment.size() << " bytes)." << std::endl;
                } else {
                    std::cout << "WS: Init segment not ready yet." << std::endl;
                }
            }
        },
        .close = [](auto *ws, int code, std::string_view message) {
            std::cout << "WS: Client disconnected." << std::endl;
        }
    })
    .get("/*", [public_dir](auto *res, auto *req) {
        std::string url(req->getUrl());
        if (url == "/") url = "/index.html";
        std::string file_path = public_dir + url;

        std::string content = readFile(file_path);
        if (content.empty()) {
            res->writeStatus("404 Not Found")->end("Not Found");
        } else {
            res->writeHeader("Content-Type", getMimeType(file_path))->end(content);
        }
    })
    .listen(port, [port, &app](auto *listen_socket) {
        if (listen_socket) {
            std::cout << "Server listening on port " << port << std::endl;

            // Timer to broadcast media segments
            app.addTimer(40, [&app](auto*){
                std::queue<std::vector<uint8_t>> segments_to_send;

                {
                    std::lock_guard<std::mutex> lock(g_media_mutex);
                    if (!g_media_segments.empty()) {
                        g_media_segments.swap(segments_to_send);
                    }
                }

                while(!segments_to_send.empty()) {
                    std::vector<uint8_t>& segment = segments_to_send.front();
                    app.publish("broadcast", std::string_view((char*)segment.data(), segment.size()), uWS::BINARY, false);
                    segments_to_send.pop();
                }
            });

        } else {
            std::cerr << "Failed to listen on port " << port << std::endl;
        }
    });

    app.run();
    return 0;
}

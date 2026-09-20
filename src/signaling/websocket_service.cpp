#include "signaling/websocket_service.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

std::shared_ptr<WebsocketService>
WebsocketService::Create(Args args, std::shared_ptr<Conductor> conductor, net::io_context &ioc) {
    return std::make_shared<WebsocketService>(args, conductor, ioc);
}

std::string WebsocketService::UrlEncode(const std::string &value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (const char c : value) {
        if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << std::uppercase << int(static_cast<unsigned char>(c));
        }
    }
    return escaped.str();
}

std::string
WebsocketService::BuildWebSocketTarget(const std::string &basePath,
                                       const std::map<std::string, std::string> &params) {
    std::ostringstream target;
    target << basePath;

    if (!params.empty()) {
        target << "?";
        bool first = true;
        for (const auto &[key, value] : params) {
            if (!first)
                target << "&";
            target << key << "=" << UrlEncode(value);
            first = false;
        }
    }
    return target.str();
}

WebsocketService::WebsocketService(Args args, std::shared_ptr<Conductor> conductor,
                                   net::io_context &ioc)
    : SignalingService(conductor),
      args_(args),
      ioc_(ioc),
      ws_(InitWebSocket(ioc)),
      resolver_(net::make_strand(ioc)),
      ping_timer_(ioc),
      reconnect_timer_(ioc),
      discovery_socket_(ioc) {}

WebsocketService::~WebsocketService() { Disconnect(); }

WebSocketVariant WebsocketService::InitWebSocket(net::io_context &ioc) {
    if (args_.use_tls) {
        // The SSL context created via boost::asio::ssl::context uses the underlying BoringSSL
        // implementation (when linked with WebRTC or other BoringSSL-based libraries). BoringSSL is
        // not a drop-in replacement for OpenSSL and does not implement all OpenSSL APIs. As a
        // result, certain methods may be unsupported or behave differently.
        // Ensure that only compatible OpenSSL APIs are used when BoringSSL is present.
        DEBUG_PRINT("Using TLS WebSocket, SSL version: %s", OpenSSL_version(OPENSSL_VERSION));

        ssl::context ctx(ssl::context::tls);
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_peer);

        return websocket::stream<ssl::stream<tcp::socket>>(net::make_strand(ioc), ctx);
    } else {
        return websocket::stream<tcp::socket>(net::make_strand(ioc));
    }
}

void WebsocketService::RecreateWebSocket() {
    // emplace (not assignment): beast's websocket stream is not move-assignable.
    if (args_.use_tls) {
        ssl::context ctx(ssl::context::tls);
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_peer);
        ws_.emplace<websocket::stream<ssl::stream<tcp::socket>>>(net::make_strand(ioc_), ctx);
    } else {
        ws_.emplace<websocket::stream<tcp::socket>>(net::make_strand(ioc_));
    }
}

void WebsocketService::Connect() {
    if (args_.ws_host == "auto") {
        StartDiscovery();
        return;
    }
    connect_host_ = args_.ws_host;
    connect_port_ = args_.ws_port != 0 ? args_.ws_port : (args_.use_tls ? 443 : 80);
    ResolveAndConnect();
}

void WebsocketService::ResolveAndConnect() {
    INFO_PRINT("Connect to WebSocket %s:%d", connect_host_.c_str(), connect_port_);

    resolver_.async_resolve(
        connect_host_, std::to_string(connect_port_),
        [this](boost::system::error_code ec, tcp::resolver::results_type results) {
            OnResolve(ec, results);
        });
}

// Multicast discovery for --ws-host=auto: listen on discovery_group/discovery_port
// (see pi-sfu's -discovery-addr) for a JSON announce, take the sender's source IP
// as the SFU host (never trust a host embedded in the payload) and its ws_port,
// then proceed exactly as a fixed --ws-host would. Re-entered on every
// (re)connect attempt while ws_host stays "auto", so a reconnect after an SFU
// restart re-discovers rather than retrying a possibly stale address.
void WebsocketService::StartDiscovery() {
    if (discovery_active_) {
        return;
    }
    // A previous attempt may have opened the socket and then failed a later step
    // (bind/join); close it first so re-opening below doesn't fail on top of that.
    boost::system::error_code ec;
    if (discovery_socket_.is_open()) {
        discovery_socket_.close(ec);
    }

    net::ip::udp::endpoint listen_ep(net::ip::udp::v4(), args_.discovery_port);
    discovery_socket_.open(listen_ep.protocol(), ec);
    if (!ec) {
        discovery_socket_.set_option(net::ip::udp::socket::reuse_address(true), ec);
    }
    if (!ec) {
        discovery_socket_.bind(listen_ep, ec);
    }
    if (!ec) {
        auto group_addr = net::ip::make_address(args_.discovery_group, ec);
        if (!ec) {
            discovery_socket_.set_option(net::ip::multicast::join_group(group_addr), ec);
        }
    }
    if (ec) {
        ERROR_PRINT("Discovery: failed to listen on %s:%d: %s", args_.discovery_group.c_str(),
                    args_.discovery_port, ec.message().c_str());
        HandleFailure("Discovery setup failed");
        return;
    }

    discovery_active_ = true;
    INFO_PRINT("Discovery: waiting for an SFU announce on %s:%d", args_.discovery_group.c_str(),
               args_.discovery_port);
    DoDiscoveryReceive();
}

void WebsocketService::StopDiscovery() {
    if (!discovery_active_) {
        return;
    }
    discovery_active_ = false;
    boost::system::error_code ec;
    discovery_socket_.close(ec);
}

void WebsocketService::DoDiscoveryReceive() {
    discovery_socket_.async_receive_from(
        net::buffer(discovery_buffer_), discovery_sender_,
        [this](boost::system::error_code ec, std::size_t bytes_transferred) {
            OnDiscoveryReceive(ec, bytes_transferred);
        });
}

void WebsocketService::OnDiscoveryReceive(boost::system::error_code ec,
                                          std::size_t bytes_transferred) {
    if (!discovery_active_) {
        return; // StopDiscovery() already closed the socket (found one / shutting down)
    }
    if (ec) {
        // Transient recv error: keep listening, the SFU re-announces every ~1.5s.
        DoDiscoveryReceive();
        return;
    }

    std::string payload(discovery_buffer_.data(), bytes_transferred);
    try {
        json beacon = json::parse(payload);
        if (beacon.value("proto", "") == "pi-sfu-discovery" && beacon.contains("ws_port")) {
            std::string host = discovery_sender_.address().to_string();
            uint16_t port = beacon["ws_port"].get<uint16_t>();
            StopDiscovery();
            INFO_PRINT("Discovery: found SFU at %s:%d", host.c_str(), port);
            connect_host_ = host;
            connect_port_ = port;
            ResolveAndConnect();
            return;
        }
    } catch (const std::exception &e) {
        DEBUG_PRINT("Discovery: ignoring unparseable announce: %s", e.what());
    }
    DoDiscoveryReceive();
}

void WebsocketService::Disconnect() {
    // Explicit disconnect (shutdown or server "leave"): stop reconnecting.
    stopping_ = true;
    ping_timer_.cancel();
    reconnect_timer_.cancel();
    StopDiscovery();

    std::visit(
        [](auto &ws) {
            if (ws.is_open()) {
                ws.async_close(websocket::close_code::normal, [](boost::system::error_code ec) {
                    if (ec) {
                        ERROR_PRINT("Close Error: %s", ec.message().c_str());
                    } else {
                        INFO_PRINT("WebSocket Closed");
                    }
                });
            } else {
                INFO_PRINT("WebSocket already closed");
            }
        },
        ws_);
}

void WebsocketService::HandleFailure(const std::string &reason) {
    if (stopping_) {
        return;
    }
    if (reconnecting_) {
        // Another failing operation already scheduled a reconnect.
        DEBUG_PRINT("%s (reconnect already pending)", reason.c_str());
        return;
    }
    ERROR_PRINT("%s; reconnecting", reason.c_str());
    ScheduleReconnect();
}

void WebsocketService::ScheduleReconnect() {
    if (stopping_) {
        return;
    }
    reconnecting_ = true;
    ping_timer_.cancel();

    // Force-close the socket so any in-flight read/write completes with an error,
    // and tear down the broken session before the next attempt.
    std::visit(
        [](auto &ws) {
            boost::system::error_code ec;
            beast::get_lowest_layer(ws).close(ec);
        },
        ws_);

    pub_peer_ = nullptr;
    sub_peer_ = nullptr;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        write_queue_.clear();
    }
    buffer_.consume(buffer_.size());

    // Exponential backoff capped at 8s: 1, 2, 4, 8, 8, ...
    int shift = reconnect_attempts_ < 3 ? reconnect_attempts_ : 3;
    int delay = 1 << shift;
    reconnect_attempts_++;
    INFO_PRINT("Reconnecting to WebSocket in %ds", delay);

    reconnect_timer_.expires_after(std::chrono::seconds(delay));
    reconnect_timer_.async_wait([this](const boost::system::error_code &ec) {
        if (ec || stopping_) {
            return;
        }
        // Attempt starts now: clear the guard so a failure in THIS attempt can
        // schedule the next one (with increased backoff).
        reconnecting_ = false;
        // Fresh stream for the new attempt; the old socket was closed above.
        RecreateWebSocket();
        Connect();
    });
}

void WebsocketService::OnResolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
        HandleFailure("Failed to resolve " + connect_host_ + ":" +
                      std::to_string(connect_port_) + ": " + ec.message());
        return;
    }

    std::visit(
        [this, results](auto &ws) {
            net::async_connect(beast::get_lowest_layer(ws), results,
                               [this, &ws](boost::system::error_code ec, tcp::endpoint) {
                                   OnConnect(ec);
                               });
        },
        ws_);
}

void WebsocketService::OnConnect(beast::error_code ec) {
    if (ec) {
        HandleFailure("Failed to connect to " + connect_host_ + ":" +
                      std::to_string(connect_port_) + ": " + ec.message());
        return;
    }

    std::visit(
        [this](auto &ws) {
            OnHandshake(ws);
        },
        ws_);
}

void WebsocketService::OnHandshake(websocket::stream<tcp::socket> &ws) {
    std::string target =
        BuildWebSocketTarget("/rtc", {{"apiKey", args_.ws_key},
                                      {"roomId", args_.ws_room},
                                      {"userId", args_.uid},
                                      {"canSubscribe", args_.enable_ipc ? "1" : "0"}});
    ws.async_handshake(connect_host_, target, [this](boost::system::error_code ec) {
        OnHandshake(ec);
    });
}

void WebsocketService::OnHandshake(websocket::stream<ssl::stream<tcp::socket>> &ws) {
    ws.next_layer().async_handshake(
        ssl::stream_base::client, [this, &ws](boost::system::error_code ec) {
            if (ec) {
                ERROR_PRINT("Failed to tls handshake with %s:%d: %s", connect_host_.c_str(),
                            connect_port_, ec.message().c_str());
            }
            std::string target =
                BuildWebSocketTarget("/rtc", {{"apiKey", args_.ws_key},
                                              {"roomId", args_.ws_room},
                                              {"userId", args_.uid},
                                              {"canSubscribe", args_.enable_ipc ? "1" : "0"}});
            ws.async_handshake(connect_host_, target, [this](boost::system::error_code ec) {
                OnHandshake(ec);
            });
        });
}

void WebsocketService::OnHandshake(beast::error_code ec) {
    if (ec) {
        HandleFailure("Failed to handshake with " + connect_host_ + ":" +
                      std::to_string(connect_port_) + ": " + ec.message());
        return;
    }

    // Connected: clear reconnect state so the next drop starts fresh backoff.
    INFO_PRINT("WebSocket connected to %s", connect_host_.c_str());
    reconnecting_ = false;
    reconnect_attempts_ = 0;

    Read();
    ScheduleNextPing();
}

void WebsocketService::Read() {
    std::visit(
        [this](auto &ws) {
            if (!ws.is_open()) {
                return;
            }

            ws.async_read(buffer_,
                          [this](boost::system::error_code ec, std::size_t bytes_transferred) {
                              if (ec) {
                                  HandleFailure("Failed to read from " + connect_host_ + ":" +
                                                std::to_string(connect_port_) + ": " +
                                                ec.message());
                                  return;
                              }
                              std::string req = beast::buffers_to_string(buffer_.data());
                              OnMessage(req);
                              buffer_.consume(bytes_transferred);
                              Read();
                          });
        },
        ws_);
}

void WebsocketService::OnMessage(const std::string &req) {
    DEBUG_PRINT("Received message: %s", req.c_str());
    json jsonObj;
    try {
        jsonObj = json::parse(req);
    } catch (const std::exception &e) {
        ERROR_PRINT("Failed to parse message: %s", e.what());
        return;
    }

    std::string action = jsonObj["action"];
    std::string message = jsonObj["message"];

    if (action == "join") {
        PeerConfig config;
        config.is_sfu_peer = true;

        webrtc::PeerConnectionInterface::IceServer ice_server;
        nlohmann::json messageJson = nlohmann::json::parse(jsonObj["message"].get<std::string>());
        ice_server.urls = messageJson["urls"].get<std::vector<std::string>>();
        ice_server.username = messageJson["username"];
        ice_server.password = messageJson["credential"];
        // Skip empty ICE servers: a server with no URLs makes libwebrtc reject the
        // whole RTCConfiguration ("Empty uri") and CreatePeer returns null. A LAN
        // SFU advertises no STUN/TURN, so the join carries an empty urls array.
        if (!ice_server.urls.empty()) {
            config.servers.push_back(ice_server);
        }

        pub_peer_ = CreatePeer(config);
        if (pub_peer_) {
            pub_peer_->OnLocalSdp([this](const std::string &peer_id, const std::string &sdp,
                                         const std::string &type) {
                Write(type, sdp);
            });
            pub_peer_->OnLocalIce([this](const std::string &peer_id, const std::string &sdp_mid,
                                         int sdp_mline_index, const std::string &candidate) {
                Write("tricklePublisher", candidate);
            });
            // The publisher peer carries all the media. When ICE/DTLS gives up, the
            // signaling TCP can stay nominally open for a long time (kernel
            // retransmits ride out the outage, and our pings just queue), so a
            // socket error - the only thing that would otherwise trigger a
            // reconnect - never arrives and this camera streams into a black hole
            // until something restarts it. Take the same teardown+reconnect path.
            pub_peer_->SetOnStateChangeCallback(
                [this](webrtc::PeerConnectionInterface::PeerConnectionState state) {
                    if (state != webrtc::PeerConnectionInterface::PeerConnectionState::kFailed) {
                        return;
                    }
                    // Called on a libwebrtc thread: hop to the io_context, which owns
                    // the socket, the timers and pub_peer_ itself.
                    net::post(ioc_, [this]() {
                        HandleFailure("Publisher peer connection failed");
                    });
                });
        }

        config.is_publisher = false;
        sub_peer_ = CreatePeer(config);
        if (sub_peer_) {
            sub_peer_->OnLocalSdp([this](const std::string &peer_id, const std::string &sdp,
                                         const std::string &type) {
                Write(type, sdp);
            });
            sub_peer_->OnLocalIce([this](const std::string &peer_id, const std::string &sdp_mid,
                                         int sdp_mline_index, const std::string &candidate) {
                Write("trickleSubscriber", candidate);
            });
        }

        Write("addVideoTrack", args_.uid);
        if (!args_.no_audio) {
            Write("addAudioTrack", args_.uid);
        }

        if (pub_peer_) {
            pub_peer_->CreateOffer();
        }

    } else if (action == "offer" && sub_peer_) {
        sub_peer_->SetRemoteSdp(message, "offer");
    } else if (action == "answer" && pub_peer_) {
        pub_peer_->SetRemoteSdp(message, "answer");
    } else if (action == "trickle") {
        OnRemoteIce(message);
    } else if (action == "leave") {
        Disconnect();
    }
}

void WebsocketService::OnRemoteIce(const std::string &message) {
    nlohmann::json res = nlohmann::json::parse(message);
    std::string target = res["target"];
    std::string canditateInit = res["candidateInit"];

    nlohmann::json canditateObj = nlohmann::json::parse(canditateInit);
    std::string sdp_mid = canditateObj["sdpMid"];
    int sdp_mline_index = canditateObj["sdpMLineIndex"];
    std::string candidate = canditateObj["candidate"];
    DEBUG_PRINT("Received remote ICE: %s, %d, %s", sdp_mid.c_str(), sdp_mline_index,
                candidate.c_str());

    if (target == "PUBLISHER") {
        pub_peer_->SetRemoteIce(sdp_mid, sdp_mline_index, candidate);
    } else if (target == "SUBSCRIBER") {
        sub_peer_->SetRemoteIce(sdp_mid, sdp_mline_index, candidate);
    }
}

void WebsocketService::ScheduleNextPing() {
    ping_timer_.expires_after(std::chrono::seconds(15));
    ping_timer_.async_wait([this](const boost::system::error_code &ec) {
        if (ec) {
            if (ec != boost::asio::error::operation_aborted) {
                ERROR_PRINT("Ping timer error: %s", ec.message().c_str());
            }
            return;
        }

        Write("ping", "");
        ScheduleNextPing();
    });
}

void WebsocketService::Write(const std::string &action, const std::string &message) {
    nlohmann::json request_json;
    request_json["action"] = action;
    request_json["message"] = message;
    std::string request = request_json.dump();

    std::lock_guard<std::mutex> lock(write_mutex_);
    bool writing_in_progress = !write_queue_.empty();
    write_queue_.push_back(request);

    if (!writing_in_progress) {
        DoWrite();
    }
}

void WebsocketService::DoWrite() {
    if (write_queue_.empty())
        return;

    std::visit(
        [this](auto &ws) {
            ws.async_write(net::buffer(write_queue_.front()),
                           [this](boost::system::error_code ec, std::size_t bytes_transferred) {
                               bool failed = false;
                               {
                                   std::lock_guard<std::mutex> lock(write_mutex_);
                                   if (ec) {
                                       failed = true;
                                   } else {
                                       write_queue_.pop_front();
                                       if (!write_queue_.empty()) {
                                           DoWrite();
                                       }
                                   }
                               }
                               if (failed) {
                                   HandleFailure("Failed to write to " + connect_host_ + ":" +
                                                 std::to_string(connect_port_) + ": " +
                                                 ec.message());
                               }
                           });
        },
        ws_);
}

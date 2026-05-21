#include "grpc_control_server.h"

#include "zfleet/core/log.h"

#include <grpcpp/grpcpp.h>

#include <stdexcept>
#include <utility>

namespace zfleet::server {

GrpcControlServer::GrpcControlServer(std::string listen_address,
                                     GrpcControlService* control_service)
    : listen_address_(std::move(listen_address)),
      control_service_(control_service) {}

GrpcControlServer::~GrpcControlServer() {
  Shutdown();
}

void GrpcControlServer::Start() {
  if (control_service_ == nullptr) {
    throw std::invalid_argument("grpc control service must not be null");
  }
  if (server_ != nullptr) {
    return;
  }

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_address_, grpc::InsecureServerCredentials(),
                           &selected_port_);
  builder.RegisterService(control_service_);
  server_ = builder.BuildAndStart();
  if (server_ == nullptr || selected_port_ == 0) {
    throw std::runtime_error("failed to start grpc control server");
  }

  ZFLOG_INFO(zfleet::core::log::Component("server").With(
                 {{"grpc_listen", listen_address_},
                  {"listen_port", std::to_string(selected_port_)}}),
             "grpc control server started");
}

void GrpcControlServer::Shutdown() {
  if (server_ != nullptr) {
    server_->Shutdown();
    server_.reset();
  }
}

void GrpcControlServer::Wait() {
  if (server_ != nullptr) {
    server_->Wait();
  }
}

std::uint16_t GrpcControlServer::port() const noexcept {
  return static_cast<std::uint16_t>(selected_port_);
}

} // namespace zfleet::server

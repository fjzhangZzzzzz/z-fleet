#pragma once

#include "grpc_control_service.h"

#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <memory>
#include <string>

namespace zfleet::server {

class GrpcControlServer {
 public:
  GrpcControlServer(std::string listen_address,
                    GrpcControlService* control_service);
  ~GrpcControlServer();

  void Start();
  void Shutdown();
  void Wait();
  std::uint16_t port() const noexcept;

 private:
  std::string listen_address_;
  GrpcControlService* control_service_;
  std::unique_ptr<grpc::Server> server_;
  int selected_port_ = 0;
};

} // namespace zfleet::server

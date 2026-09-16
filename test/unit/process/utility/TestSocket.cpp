#include "process/utility/Socket.h"
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

int main() {
  asio::io_context context;
  asio::ip::tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
  const auto port = acceptor.local_endpoint().port();
  std::string payload(2 * 1024 * 1024, '\0');
  for (size_t i=0; i<payload.size(); ++i) payload[i]=char(i%251);
  auto received = std::async(std::launch::async, [&] {
    asio::ip::tcp::socket peer(context);
    acceptor.accept(peer);
    peer.non_blocking(true);
    std::string all;
    char buffer[4096];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (all.size()<payload.size() && std::chrono::steady_clock::now()<deadline) {
      asio::error_code error;
      const auto n=peer.read_some(asio::buffer(buffer),error);
      if (!error) all.append(buffer,n);
      else if(error!=asio::error::would_block && error!=asio::error::try_again)
        throw std::runtime_error("Unexpected loopback receive error");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return all;
  });
  Socket sender("127.0.0.1",port);
  sender.sendData("");
  sender.sendData(payload);
  if(received.get()!=payload)throw std::runtime_error("Complete binary frame changed or truncated");

  asio::ip::tcp::acceptor failureAcceptor(context,{asio::ip::address_v4::loopback(),0});
  auto reset = std::async(std::launch::async,[&]{
    asio::ip::tcp::socket peer(context);failureAcceptor.accept(peer);
    peer.set_option(asio::socket_base::linger(true,0));peer.close();
  });
  Socket broken("127.0.0.1",failureAcceptor.local_endpoint().port());
  reset.get();
  bool failed=false;
  try { broken.sendData(payload); } catch (const std::exception&) { failed=true; }
  if(!failed)throw std::runtime_error("Failed frame was reported as successful");
}

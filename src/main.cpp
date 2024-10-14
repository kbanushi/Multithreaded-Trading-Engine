#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <string>
#include <thread>
#include <deque>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;
using json = nlohmann::json;

#define PERIOD_MAX 100

typedef struct PriceVolume {
    std::string symbol;
    double price;
    double volume;

    PriceVolume(std::string s, double p, double v) : symbol(s), price(p), volume(v) {};
} PriceVolume;

std::string load_api_token(const std::string& file_path) {
    YAML::Node config = YAML::LoadFile(file_path);
    if (config["api_token"]) {
        return config["api_token"].as<std::string>();
    } else {
        throw std::runtime_error("API token not found in the YAML file");
    }
}

// Function to send subscriptions to symbols
void send_subscriptions(websocket::stream<ssl::stream<tcp::socket>>& ws) {
    std::string appleSubscription = "{\"type\":\"subscribe\",\"symbol\":\"AAPL\"}";
    ws.write(net::buffer(appleSubscription));
}


void handle_message(std::deque<PriceVolume>& prices, const std::string& message) {
    try {
        json parsed = json::parse(message);

        // Check if it's an error message or data message
        if (parsed.contains("type")) {
            std::string type = parsed["type"];
            if (type == "error") {
                std::cout << "Error: " << parsed["msg"] << std::endl;
            } else if (type == "trade") {
                auto trade_data = parsed["data"];
                for (const auto& trade : trade_data) {
                    std::string symbol = trade["s"];
                    double price = trade["p"];
                    int volume = trade["v"];

                    if (prices.size() == PERIOD_MAX){
                        prices.pop_front();
                    }

                    prices.push_back(PriceVolume(symbol, price, volume));
                }
            }
        }
    } catch (const json::parse_error& e) {
        std::cerr << "JSON Parse Error: " << e.what() << std::endl;
    }
}

double calculateSMA(const std::deque<PriceVolume>& priceVolumes, int period) {
    double sum = 0.0;
    for (int i = 0; i < period; ++i) {
        sum += priceVolumes[i].price;
    }
    return sum / period;
}

int main() {
    try {
        std::string token = load_api_token("config.yaml");

        const std::string host = "ws.finnhub.io";
        const std::string port = "443";
        const std::string target = "/?token=" + token;

        // Set up the io_context and SSL context
        net::io_context ioc;
        ssl::context ctx(ssl::context::tlsv12_client);

        // Set recommended SSL options
        ctx.set_options(
            ssl::context::default_workarounds |
            ssl::context::no_sslv2 |
            ssl::context::no_sslv3 |
            ssl::context::single_dh_use);

        ctx.set_default_verify_paths();

        tcp::resolver resolver(ioc);
        websocket::stream<ssl::stream<tcp::socket>> ws(ioc, ctx);

        // Look up the domain name
        auto const results = resolver.resolve(host, port);

        // Make the connection to the WebSocket server
        net::connect(ws.next_layer().next_layer(), results.begin(), results.end());

        // SNI for the SSL handshake
        if(!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str())) {
            throw beast::system_error(
                beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()),
                "Failed to set SNI Host Name");
        }

        // Perform the SSL handshake
        ws.next_layer().handshake(ssl::stream_base::client);

        // Perform the WebSocket handshake
        ws.handshake(host, target);

        send_subscriptions(ws);

        // Run a separate thread to handle reading data
        std::thread read_thread([&]() {
            beast::flat_buffer buffer;
            std::string message;
            std::deque<PriceVolume> priceVolumes;

            while (true) {
                ws.read(buffer);

                message = beast::buffers_to_string(buffer.data());

                handle_message(priceVolumes, message);

                if (priceVolumes.size() == PERIOD_MAX){
                    double sma = calculateSMA(priceVolumes, PERIOD_MAX);

                    std::cout << "SMA: " << sma << std::endl;
                }

                // Clear the buffer for the next message
                buffer.consume(buffer.size());
            }
        });

        // Main thread stays alive and waits for the read thread to complete
        read_thread.join();

        // Close the WebSocket connection
        ws.close(websocket::close_code::normal);

    } catch (const beast::system_error& se) {
        std::cerr << "Error: " << se.code().message() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    return 0;
}

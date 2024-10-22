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
#include <unordered_map>
#include <queue>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;
using json = nlohmann::json;

#define PERIOD_MAX 100

std::mutex printMtx;

typedef struct PriceVolume{
    double price;
    double volume;

    PriceVolume(double p, double v) : price(p), volume(v) {};
} PriceVolume;

class EMATrade{
private:
    double value;
    double earnings;
    double priceBought;
    bool holdingStock;

    void calculateSMA(const std::deque<PriceVolume>& priceVolumes, int period){
        double sum = 0.0;

        for (int i = priceVolumes.size() - period; i < period; ++i) {
            sum += priceVolumes[i].price;
        }

        value = sum / period;
    }

    void updateEMA(double price, int period){
        double prevEMA = value;
        double alpha = 2.0 / (period + 1);
        value = price * alpha + prevEMA * (1 - alpha);
    }
public:
    EMATrade(){
        value = 0;
        earnings = 0;
        priceBought = 0;
        holdingStock = false;
    }

    void evaluateTrade(const std::deque<PriceVolume>& priceVolumes, double price, int period){
        if (value == 0){
            calculateSMA(priceVolumes, period);
        }
        else{
            updateEMA(price, period);
        }

        if (price < value && !holdingStock){ //Current price is below EMA -> buy a share
            earnings -= price;
            holdingStock = true;
            priceBought = price;
        }
        else if (price > value && holdingStock){ //Sell share
            earnings += price;
            holdingStock = false;
        }
    }

    double getEarnings() const {
        return earnings;
    }
};

class MomentumTrade{
    private:
        double earnings;
        double priceBought;
        double roc;
        bool holdingStock;

    public:
        MomentumTrade(){
            roc = 0;
            earnings = 0;
            priceBought = 0;
            holdingStock = false;
        }
        
        double calculateROC(const std::deque<PriceVolume>& priceVolumes, int period){
            int currIndex = priceVolumes.size() - 1;

            //Note deque structure where newest values are pushed to the back and old values are popped from front
            double priceThen = priceVolumes[currIndex - period].price;
            double priceNow = priceVolumes[currIndex].price;

            return ((priceNow - priceThen) / priceThen) * 100;
        }

        void evaluateTrade(const std::deque<PriceVolume>& priceVolumes, double price, double threshold, int period){
            roc = calculateROC(priceVolumes, period);

            if (roc > threshold && !holdingStock){
                earnings -= price;
                priceBought = price;
                holdingStock = true;
            }
            else if (roc < threshold && holdingStock){
                earnings += price;
                holdingStock = false;
            }
        }

        double getEarnings() const {
            return earnings;
        }
};

typedef struct Stock{
    std::deque<PriceVolume> priceVolumes;
    EMATrade emaTrade;
    MomentumTrade momentumTrade;
    std::mutex mtx;
    std::queue<std::function<void()>> taskQueue;
    std::condition_variable cv;
    std::atomic<bool> stopFlag = false;
} Stock;

void loadYAMLFile(const std::string& filePath, std::string& token, std::vector<std::string>& symbols);
void sendSubscriptions(websocket::stream<ssl::stream<tcp::socket>>& ws, const std::vector<std::string>& symbols);
void handleTrade(const std::string& symbol, Stock& stock, const double& price, const double& volume);
void handleMessage(std::unordered_map<std::string, Stock>& symbolMap, const std::string& message);
void readThread(websocket::stream<ssl::stream<tcp::socket>>& ws);

void stockThreadWorker(Stock& stock) {
    while (!stock.stopFlag) {
        std::lock_guard<std::mutex> lock(stock.mtx);

        // Wait for a task to be added to the queue or stop signal
        stock.cv.wait(lock, [&stock] { return !stock.taskQueue.empty() || stock.stopFlag; });

        // Process tasks in the queue
        while (!stock.taskQueue.empty()) {
            auto task = std::move(stock.taskQueue.front());
            stock.taskQueue.pop();
            lock.unlock();
            task();  //handleTrade(...)
            lock.lock();
        }
    }
}

void postTaskToStock(Stock& stock, std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(stock.mtx);
        stock.taskQueue.push(task);
    }
    stock.cv.notify_one();  // Notify the thread there’s a new task
}

void loadYAMLFile(const std::string& filePath, std::string& token, std::vector<std::string>& symbols){
    YAML::Node config = YAML::LoadFile(filePath);

    if (config["api_token"]) {
        token = config["api_token"].as<std::string>();
    } else {
        throw std::runtime_error("API token not found in the YAML file");
    }

    if (config["symbols"]) {
        for (const auto& item : config["symbols"]) {
            symbols.push_back(item.as<std::string>());
        }
    } else {
        throw std::runtime_error("Symbols array not found in YAML file");
    }
}

void sendSubscriptions(websocket::stream<ssl::stream<tcp::socket>>& ws, const std::vector<std::string>& symbols){
    for (const std::string& symbol : symbols){
        std::string subscription = std::format("{{\"type\":\"subscribe\",\"symbol\":\"{}\"}}", symbol);
        ws.write(net::buffer(subscription));
    }
}

void printStandings(const std::string& symbol, const Stock& stock){
    std::lock_guard<std::mutex> lock(printMtx);

    std::cout << "Current standing for stock: " << symbol << std::endl;
    std::cout << "EMA Trading profits: " << stock.emaTrade.getEarnings() << std::endl;
    std::cout << "Momenum Trading profits: " << stock.momentumTrade.getEarnings() << std::endl;
}

void handleTrade(const std::string& symbol, Stock& stock, const double& price, const double& volume){
    if (stock.priceVolumes.size() > PERIOD_MAX){
        stock.emaTrade.evaluateTrade(stock.priceVolumes, price, PERIOD_MAX);
        stock.momentumTrade.evaluateTrade(stock.priceVolumes, price, 0, PERIOD_MAX);

        printStandings(symbol, stock);
    }

    if (stock.priceVolumes.size() == 3 * PERIOD_MAX){
        stock.priceVolumes.pop_front();
    }

    stock.priceVolumes.push_back(PriceVolume(price, volume));
}

void handleMessage(std::unordered_map<std::string, Stock>& symbolMap, const std::string& message){
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

                    // Stock has not been seen yet, therefore create thread to support stock operations
                    if (!symbolMap.count(symbol)){
                        std::thread stock_thread([&]{
                            stockThreadWorker(symbolMap[symbol]);
                        });

                        stock_thread.detach(); // Do not wait for thread to complete
                    }

                    postTaskToStock(symbolMap[symbol], [&]{
                        handleTrade(symbol, symbolMap[symbol], price, volume);
                    });
                }
            }
        }
    } catch (const json::parse_error& e) {
        std::cerr << "JSON Parse Error: " << e.what() << std::endl;
    }
}

void readThread(websocket::stream<ssl::stream<tcp::socket>>& ws){
    beast::flat_buffer buffer;
    std::string message;
    std::unordered_map<std::string, Stock> symbolMap;

    while (true) {
        ws.read(buffer);
        message = beast::buffers_to_string(buffer.data());
        handleMessage(symbolMap, message);
        buffer.consume(buffer.size());
    }
}

int main() {
    try {
        std::string token;
        std::vector<std::string> symbols;
        loadYAMLFile("config.yaml", token, symbols);
        
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
        if(!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str())){
            throw beast::system_error(
                beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()),
                "Failed to set SNI Host Name");
        }

        // Perform the SSL handshake
        ws.next_layer().handshake(ssl::stream_base::client);

        // Perform the WebSocket handshake
        ws.handshake(host, target);

        sendSubscriptions(ws, symbols);

        // Run a separate thread to handle reading data
        std::thread read_thread([&ws](){
            readThread(ws);
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

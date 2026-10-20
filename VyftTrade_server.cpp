// VyftTrade_server.cpp : API Vyft Trade™ (vyft-trade.vyft-one.com) — prise d'ordres VEZ
//
//   POST /api/v1/trade                    ordre au format du moteur (execute_order | cancel_order)
//   GET  /api/v1/trade/orders?wallet=0x…  ordres d'un sous-wallet VEZ (plus récents en premier)
//   GET  /api/v1/trade/orders/<orderid>   un ordre
//   GET  /health                          état du service
//
// Lancement : VyftTradeServer [port=8080] [fichier=orders.json]
// Clé d'API facultative : VYFTTRADE_API_KEY (si définie, l'en-tête X-API-KEY est exigé).
// Le TLS est assuré par le proxy frontal (vyft-trade.vyft-one.com → ce service en HTTP).
//
// Règlement : les ordres acceptés sont enregistrés « pending ». Leur exécution sur Slura
// (livraison ou réception des VEZ, paiement en devise) reste à brancher dans settleOrder().

#include "VyftTrade_motor.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>

using json = nlohmann::json;

namespace {

std::string nowIso() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string newOrderId() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream out;
    out << "vt_" << std::hex << std::setfill('0') << std::setw(16) << rng() << std::setw(16) << rng();
    return out.str();
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// Champ texte ou nombre → texte (l'app envoie les quantités en chaînes, d'autres clients en nombres)
std::string field(const json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) return {};
    const json& v = object.at(key);
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) return v.dump();
    return {};
}

// Carnet d'ordres persistant (fichier JSON réécrit à chaque modification)
class OrderStore {
public:
    explicit OrderStore(std::filesystem::path file) : file_(std::move(file)) {
        std::ifstream in(file_);
        if (in) {
            try {
                in >> orders_;
            } catch (...) {
                orders_ = json::array();
            }
        }
        if (!orders_.is_array()) orders_ = json::array();
    }

    json add(json order) {
        std::lock_guard<std::mutex> lock(mutex_);
        orders_.push_back(order);
        save();
        return order;
    }

    std::optional<json> find(const std::string& orderId) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& o : orders_)
            if (o.value("orderid", "") == orderId) return o;
        return std::nullopt;
    }

    json byWallet(const std::string& wallet) {
        std::lock_guard<std::mutex> lock(mutex_);
        json out = json::array();
        for (auto it = orders_.rbegin(); it != orders_.rend(); ++it)
            if (lower(it->value("wallet", "")) == lower(wallet)) out.push_back(*it);
        return out;
    }

    // Annule un ordre « pending » du même wallet ; renvoie le code HTTP et l'ordre ou l'erreur
    std::pair<int, json> cancel(const std::string& orderId, const std::string& wallet) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& o : orders_) {
            if (o.value("orderid", "") != orderId) continue;
            if (lower(o.value("wallet", "")) != lower(wallet))
                return {403, {{"error", "Cet ordre n'appartient pas à ce sous-wallet"}}};
            if (o.value("status", "") != "pending")
                return {409, {{"error", "Ordre déjà " + o.value("status", "traité")}}};
            o["status"] = "cancelled";
            o["updated_at"] = nowIso();
            save();
            return {200, o};
        }
        return {404, {{"error", "Ordre introuvable"}}};
    }

private:
    void save() {
        const auto tmp = file_.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            out << orders_.dump(2);
        }
        std::filesystem::rename(tmp, file_);
    }

    std::filesystem::path file_;
    json orders_ = json::array();
    std::mutex mutex_;
};

// Point d'accroche du règlement sur Slura (non branché : l'ordre reste « pending »)
void settleOrder(json&) {}

void reply(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

}  // namespace

int main(int argc, char** argv) {
    const int port = argc > 1 ? std::atoi(argv[1]) : 8080;
    const std::filesystem::path dataFile = argc > 2 ? argv[2] : "orders.json";
    const char* apiKeyEnv = std::getenv("VYFTTRADE_API_KEY");
    const std::string apiKey = apiKeyEnv ? apiKeyEnv : "";

    OrderStore store(dataFile);
    httplib::Server server;

    // Clé d'API (si configurée) sur toutes les routes /api
    server.set_pre_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
        if (!apiKey.empty() && req.path.rfind("/api/", 0) == 0 && req.get_header_value("X-API-KEY") != apiKey) {
            reply(res, 401, {{"error", "Clé d'API invalide"}});
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        reply(res, 200, {{"status", "ok"}, {"asset", vyfttrade::kVezAsset}, {"chain", vyfttrade::kVezChain}});
    });

    server.Post("/api/v1/trade", [&](const httplib::Request& req, httplib::Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            reply(res, 400, {{"error", "Corps JSON invalide"}});
            return;
        }
        const json assetdata = body.value("assetdata", json::object());
        const json action = body.value("action", json::object());

        auto trade = vyfttrade::parseTradeType(field(action, "typetrade"));
        auto actionType = vyfttrade::parseActionType(field(action, "typeaction"));
        if (!trade || !actionType) {
            reply(res, 400, {{"error", "typetrade (buy|sell) ou typeaction (execute_order|cancel_order) invalide"}});
            return;
        }

        char* end = nullptr;
        const std::string quantityText = field(action, "quantity");
        const double quantity = std::strtod(quantityText.c_str(), &end);
        const std::string priceText = field(action, "priceincurrency");
        const double price = priceText.empty() ? vyfttrade::kVezPriceEur : std::strtod(priceText.c_str(), nullptr);
        const std::string currency = field(action, "currency").empty() ? "EUR" : field(action, "currency");

        vyfttrade::Order order =
            vyfttrade::makeVezOrder(*trade, *actionType, quantityText.empty() || end == quantityText.c_str() ? 0 : quantity,
                                    field(action, "wallet"), currency, price);
        if (!field(assetdata, "asset").empty()) order.assetData.asset = field(assetdata, "asset");
        if (!field(assetdata, "chain").empty()) order.assetData.chain = field(assetdata, "chain");
        order.action.orderId = field(action, "orderid");

        if (auto error = vyfttrade::validateOrder(order)) {
            reply(res, 422, {{"error", *error}});
            return;
        }

        if (*actionType == vyfttrade::ActionType::CancelOrder) {
            auto [status, result] = store.cancel(order.action.orderId, order.action.wallet);
            reply(res, status, result);
            return;
        }

        json saved = {
            {"orderid", newOrderId()},
            {"status", "pending"},
            {"asset", order.assetData.asset},
            {"chain", order.assetData.chain},
            {"typetrade", vyfttrade::toString(order.action.tradeType)},
            {"quantity", vyfttrade::formatQuantity(order.action.quantity)},
            {"currency", order.action.currency},
            {"priceincurrency", priceText.empty() ? "1" : priceText},
            {"total", vyfttrade::formatQuantity(order.action.quantity * order.action.priceInCurrency)},
            {"wallet", order.action.wallet},
            {"created_at", nowIso()},
            {"updated_at", nowIso()},
        };
        settleOrder(saved);
        reply(res, 201, store.add(saved));
    });

    server.Get("/api/v1/trade/orders", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string wallet = req.get_param_value("wallet");
        if (wallet.size() != 42 || wallet.rfind("0x", 0) != 0) {
            reply(res, 400, {{"error", "Paramètre wallet (0x + 40 caractères) requis"}});
            return;
        }
        reply(res, 200, {{"orders", store.byWallet(wallet)}});
    });

    server.Get(R"(/api/v1/trade/orders/([A-Za-z0-9_]+))", [&](const httplib::Request& req, httplib::Response& res) {
        if (auto order = store.find(req.matches[1])) reply(res, 200, *order);
        else reply(res, 404, {{"error", "Ordre introuvable"}});
    });

    std::cout << "Vyft Trade API sur le port " << port << " (ordres : " << dataFile.string() << ")" << std::endl;
    if (!server.listen("0.0.0.0", port)) {
        std::cerr << "Impossible d'écouter sur le port " << port << std::endl;
        return 1;
    }
    return 0;
}

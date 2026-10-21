// VyftTrade_server.cpp : API Vyft Trade™ (vyft-trade.vyft-one.com) — positions VEZ à effet de levier
//
//   POST /api/v1/trade                         execute_order : ouvre une position (mise, levier, stop loss)
//                                              cancel_order  : clôt la position orderid au prix courant
//   GET  /api/v1/trade/quote?wallet=0x…&stake=N prix, offre disponible, levier maximum proposé
//   GET  /api/v1/trade/orders?wallet=0x…       positions d'un sous-wallet VEZ (plus récentes en premier)
//   GET  /api/v1/trade/orders/<orderid>        une position
//   GET  /health                               état du service
//
// Lancement : VyftTradeServer [port=8080] [fichier=orders.json]
//   VYFTTRADE_API_KEY       facultatif : exige l'en-tête X-API-KEY sur /api
//   VYFTTRADE_SETTLER_URL   service de règlement (défaut http://127.0.0.1:8090)
//   SETTLER_TOKEN           facultatif : jeton partagé avec le service de règlement
//   VYFTTRADE_POLL_SECONDS  intervalle de surveillance des stop loss (défaut 15)
// Le TLS est assuré par le proxy frontal (vyft-trade.vyft-one.com → ce service en HTTP).
//
// Règles (voir VyftTrade_motor.h) : la mise reste sur le wallet du client ; au stop loss la position
// est fermée sans perte ni gain ; un gain est émis en VEZ sur l'offre totale (POST /api/mint de la preuve de réserves).

#include "VyftTrade_motor.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

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
    static std::mutex rngMutex;
    std::lock_guard<std::mutex> lock(rngMutex);
    std::ostringstream out;
    out << "vt_" << std::hex << std::setfill('0') << std::setw(16) << rng() << std::setw(16) << rng();
    return out.str();
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// Nombre → texte sans zéros inutiles (prix VEZ très petits : 10 décimales)
std::string decimal(double value, int decimals = 10) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

double number(const json& object, const char* key, double fallback = 0) {
    if (!object.is_object() || !object.contains(key)) return fallback;
    const json& v = object.at(key);
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        char* end = nullptr;
        const double d = std::strtod(s.c_str(), &end);
        if (!s.empty() && end != s.c_str()) return d;
    }
    return fallback;
}

// Champ texte ou nombre → texte (l'app envoie les quantités en chaînes, d'autres clients en nombres)
std::string field(const json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) return {};
    const json& v = object.at(key);
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) return v.dump();
    return {};
}

bool isWallet(const std::string& w) {
    return w.size() == 42 && w.rfind("0x", 0) == 0 &&
           std::all_of(w.begin() + 2, w.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

vyfttrade::Position toPosition(const json& p) {
    vyfttrade::Position pos;
    pos.side = p.value("typetrade", "buy") == "sell" ? vyfttrade::TradeType::Sell : vyfttrade::TradeType::Buy;
    pos.stake = number(p, "stake");
    pos.leverage = number(p, "leverage", 1);
    pos.entryPrice = number(p, "entryprice");
    pos.stopLoss = number(p, "stoploss");
    return pos;
}

// ── Service de règlement (émission via la preuve de réserves VEzcustody) ──
class Settler {
public:
    Settler(std::string url, std::string token) : url_(std::move(url)), token_(std::move(token)) {}

    std::optional<double> price() {
        auto body = get("/price");
        return body ? std::optional<double>(number(*body, "price")) : std::nullopt;
    }

    // VEZ encore émissibles (availableToMint de la preuve de réserves ; nullopt si injoignable)
    std::optional<double> mintable() {
        auto body = get("/mintable");
        return body ? std::optional<double>(number(*body, "balance")) : std::nullopt;
    }

    std::optional<double> balance(const std::string& wallet) {
        auto body = get("/balance?wallet=" + wallet);
        return body ? std::optional<double>(number(*body, "balance")) : std::nullopt;
    }

    // Versement idempotent (ref = orderid) : code HTTP du service et sa réponse
    std::pair<int, json> payout(const std::string& ref, const std::string& to, const std::string& amount) {
        const json request = {{"ref", ref}, {"to", to}, {"amount", amount}};
        auto client = makeClient(150);   // un versement attend sa confirmation sur la chaîne
        auto res = client.Post("/payout", headers(), request.dump(), "application/json");
        if (!res) return {0, {{"error", "Service de règlement injoignable"}}};
        try {
            return {res->status, json::parse(res->body)};
        } catch (...) {
            return {res->status, {{"error", res->body}}};
        }
    }

private:
    httplib::Headers headers() const {
        return token_.empty() ? httplib::Headers{} : httplib::Headers{{"X-SETTLER-TOKEN", token_}};
    }

    // Un client par appel : un versement long ne bloque ni les prix ni les devis
    httplib::Client makeClient(int readTimeoutSeconds) const {
        httplib::Client client(url_);
        client.set_connection_timeout(5);
        client.set_read_timeout(readTimeoutSeconds);
        return client;
    }

    std::optional<json> get(const std::string& path) {
        auto client = makeClient(30);
        auto res = client.Get(path, headers());
        if (!res || res->status != 200) return std::nullopt;
        try {
            return json::parse(res->body);
        } catch (...) {
            return std::nullopt;
        }
    }

    std::string url_;
    std::string token_;
};

// Statuts qui gardent un gain réservé sur l'offre émissible
bool reservesGain(const std::string& status) {
    return status == "open" || status == "closing" || status == "payout_pending" || status == "payout_review";
}

// ── Positions persistantes (fichier JSON réécrit à chaque modification) ──
class PositionStore {
public:
    explicit PositionStore(std::filesystem::path file) : file_(std::move(file)) {
        std::ifstream in(file_);
        if (in) {
            try {
                in >> items_;
            } catch (...) {
                items_ = json::array();
            }
        }
        if (!items_.is_array()) items_ = json::array();
        // Clôture interrompue par un arrêt : on la reprend au prochain passage de la surveillance
        for (auto& p : items_)
            if (p.value("status", "") == "closing") p["status"] = "payout_pending";
    }

    double reserved() {
        std::lock_guard<std::mutex> lock(mutex_);
        return reservedLocked();
    }

    double openStake(const std::string& wallet) {
        std::lock_guard<std::mutex> lock(mutex_);
        return openStakeLocked(wallet);
    }

    // Ouvre la position si l'offre émissible et le solde du client la couvrent encore (vérifié sous verrou)
    std::pair<int, json> open(json position, double mintable, double walletBalance) {
        std::lock_guard<std::mutex> lock(mutex_);
        const vyfttrade::Position pos = toPosition(position);
        const double available = std::max(0.0, mintable - reservedLocked());
        if (auto error = vyfttrade::validatePosition(pos, available)) return {422, {{"error", *error}}};
        const std::string wallet = position.value("wallet", "");
        if (openStakeLocked(wallet) + pos.stake > walletBalance + 1e-9)
            return {422, {{"error", "Solde VEZ insuffisant pour cette mise (positions ouvertes comprises)"}}};
        items_.push_back(position);
        save();
        return {201, position};
    }

    std::optional<json> find(const std::string& orderId) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& p : items_)
            if (p.value("orderid", "") == orderId) return p;
        return std::nullopt;
    }

    json byWallet(const std::string& wallet) {
        std::lock_guard<std::mutex> lock(mutex_);
        json out = json::array();
        for (auto it = items_.rbegin(); it != items_.rend(); ++it)
            if (lower(it->value("wallet", "")) == lower(wallet)) out.push_back(*it);
        return out;
    }

    std::vector<std::string> idsWithStatus(const std::string& status) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> ids;
        for (const auto& p : items_)
            if (p.value("status", "") == status) ids.push_back(p.value("orderid", ""));
        return ids;
    }

    // Passe une position de `from` à « closing » (un seul appelant gagne) ; wallet vide = sans contrôle
    std::pair<int, json> beginClose(const std::string& orderId, const std::string& wallet, const std::string& from) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : items_) {
            if (p.value("orderid", "") != orderId) continue;
            if (!wallet.empty() && lower(p.value("wallet", "")) != lower(wallet))
                return {403, {{"error", "Cette position n'appartient pas à ce sous-wallet"}}};
            if (p.value("status", "") != from) return {409, {{"error", "Position déjà " + p.value("status", "traitée")}}};
            p["status"] = "closing";
            p["updated_at"] = nowIso();
            save();
            return {200, p};
        }
        return {404, {{"error", "Position introuvable"}}};
    }

    json update(const std::string& orderId, const std::function<void(json&)>& change) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : items_) {
            if (p.value("orderid", "") != orderId) continue;
            change(p);
            p["updated_at"] = nowIso();
            save();
            return p;
        }
        return {};
    }

private:
    double reservedLocked() const {
        double total = 0;
        for (const auto& p : items_)
            if (reservesGain(p.value("status", ""))) total += number(p, "maxgain");
        return total;
    }

    double openStakeLocked(const std::string& wallet) const {
        double total = 0;
        for (const auto& p : items_)
            if (p.value("status", "") == "open" && lower(p.value("wallet", "")) == lower(wallet))
                total += number(p, "stake");
        return total;
    }

    void save() {
        const auto tmp = file_.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            out << items_.dump(2);
        }
        std::filesystem::rename(tmp, file_);
    }

    std::filesystem::path file_;
    json items_ = json::array();
    std::mutex mutex_;
};

// Verse le résultat d'une position en « closing » (ou relance un versement en attente)
json settle(PositionStore& store, Settler& settler, const json& position) {
    const std::string id = position.value("orderid", "");
    const double amount = number(position, "payout");
    if (amount <= 0) {
        // Stop loss ou position sans gain : rien n'est perdu ni versé
        return store.update(id, [](json& p) {
            p["status"] = "closed";
            p["result"] = "nul";
        });
    }
    auto [status, body] = settler.payout(id, position.value("wallet", ""), position.value("payout", "0"));
    return store.update(id, [&, status = status, body = body](json& p) {
        if (status == 200) {
            p["status"] = "closed";
            p["result"] = "gain";
            p["txhash"] = body.value("txHash", "");
            p.erase("error");
        } else if (status == 409) {
            p["status"] = "payout_review";   // état incertain : aucun nouveau versement automatique
            p["error"] = body.value("error", "Versement à vérifier");
        } else {
            p["status"] = "payout_pending";  // relancé par la surveillance (même ref, jamais payé deux fois)
            p["error"] = body.value("error", "Versement en attente");
        }
    });
}

// Clôture : calcule le versement au prix courant, puis règle
json closePosition(PositionStore& store, Settler& settler, json position, double price,
                   vyfttrade::CloseReason reason) {
    const vyfttrade::Position pos = toPosition(position);
    const double amount = vyfttrade::settlementAmount(pos, price, reason);
    position = store.update(position.value("orderid", ""), [&](json& p) {
        p["closeprice"] = decimal(price);
        p["closereason"] = vyfttrade::toString(reason);
        p["closed_at"] = nowIso();
        p["payout"] = vyfttrade::formatQuantity(amount);
    });
    return settle(store, settler, position);
}

void reply(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

}  // namespace

int main(int argc, char** argv) {
    const int port = argc > 1 ? std::atoi(argv[1]) : 8080;
    const std::filesystem::path dataFile = argc > 2 ? argv[2] : "orders.json";
    auto envOr = [](const char* name, const char* fallback) {
        const char* v = std::getenv(name);
        return std::string(v && *v ? v : fallback);
    };
    const std::string apiKey = envOr("VYFTTRADE_API_KEY", "");
    const int pollSeconds = std::max(5, std::atoi(envOr("VYFTTRADE_POLL_SECONDS", "15").c_str()));

    PositionStore store(dataFile);
    Settler settler(envOr("VYFTTRADE_SETTLER_URL", "http://127.0.0.1:8090"), envOr("SETTLER_TOKEN", ""));
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

    // Prix, offre disponible et levier maximum proposé pour une mise
    server.Get("/api/v1/trade/quote", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string wallet = req.get_param_value("wallet");
        const double stake = std::strtod(req.get_param_value("stake").c_str(), nullptr);
        auto price = settler.price();
        if (!price || *price <= 0) {
            reply(res, 503, {{"error", "Prix VEZ indisponible"}});
            return;
        }
        const double mintable = settler.mintable().value_or(0);
        const double reserved = store.reserved();
        const double available = std::max(0.0, mintable - reserved);
        const double maxLev = vyfttrade::maxLeverage(stake, available);
        json quote = {
            {"price", decimal(*price)},
            {"mintable", vyfttrade::formatQuantity(mintable)},
            {"reserved", vyfttrade::formatQuantity(reserved)},
            {"available", vyfttrade::formatQuantity(available)},
            {"gaincapmove", decimal(vyfttrade::kGainCapMove, 4)},
            {"maxleverage", decimal(maxLev, 0)},
            {"suggestedleverage", decimal(maxLev, 0)},
        };
        if (isWallet(wallet)) {
            quote["balance"] = vyfttrade::formatQuantity(settler.balance(wallet).value_or(0));
            quote["openstake"] = vyfttrade::formatQuantity(store.openStake(wallet));
        }
        reply(res, 200, quote);
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

        const std::string currency = field(action, "currency").empty() ? "EUR" : field(action, "currency");
        vyfttrade::Order order = vyfttrade::makeVezOrder(*trade, *actionType, number(action, "quantity"),
                                                         field(action, "wallet"), currency,
                                                         number(action, "priceincurrency", vyfttrade::kVezPriceEur));
        if (!field(assetdata, "asset").empty()) order.assetData.asset = field(assetdata, "asset");
        if (!field(assetdata, "chain").empty()) order.assetData.chain = field(assetdata, "chain");
        order.action.orderId = field(action, "orderid");
        order.action.leverage = number(action, "leverage", 1);
        order.action.stopLoss = number(action, "stoploss", 0);

        // Clôture : la quantité n'est pas utile
        if (*actionType == vyfttrade::ActionType::CancelOrder && order.action.quantity <= 0) order.action.quantity = 1;
        if (auto error = vyfttrade::validateOrder(order)) {
            reply(res, 422, {{"error", *error}});
            return;
        }

        auto price = settler.price();
        if (!price || *price <= 0) {
            reply(res, 503, {{"error", "Prix VEZ indisponible (service de règlement)"}});
            return;
        }

        if (*actionType == vyfttrade::ActionType::CancelOrder) {
            auto [status, position] = store.beginClose(order.action.orderId, order.action.wallet, "open");
            if (status != 200) {
                reply(res, status, position);
                return;
            }
            reply(res, 200, closePosition(store, settler, position, *price, vyfttrade::CloseReason::None));
            return;
        }

        auto balance = settler.balance(order.action.wallet);
        auto mintable = settler.mintable();
        if (!balance || !mintable) {
            reply(res, 503, {{"error", "Solde ou offre émissible indisponible (service de règlement)"}});
            return;
        }

        vyfttrade::Position pos;
        pos.side = order.action.tradeType;
        pos.stake = order.action.quantity;
        pos.leverage = order.action.leverage;
        pos.entryPrice = *price;
        pos.stopLoss = order.action.stopLoss > 0 ? order.action.stopLoss
                                                 : vyfttrade::autoStopLoss(pos.side, pos.entryPrice, pos.leverage);
        const double takeProfit = pos.side == vyfttrade::TradeType::Buy ? pos.entryPrice * (1 + vyfttrade::kGainCapMove)
                                                                        : pos.entryPrice * (1 - vyfttrade::kGainCapMove);
        json position = {
            {"orderid", newOrderId()},
            {"status", "open"},
            {"asset", order.assetData.asset},
            {"chain", order.assetData.chain},
            {"typetrade", vyfttrade::toString(pos.side)},
            {"stake", vyfttrade::formatQuantity(pos.stake)},
            {"leverage", decimal(pos.leverage, 2)},
            {"entryprice", decimal(pos.entryPrice)},
            {"stoploss", decimal(pos.stopLoss)},
            {"takeprofit", decimal(takeProfit)},
            {"maxgain", vyfttrade::formatQuantity(vyfttrade::maxGain(pos.stake, pos.leverage))},
            {"currency", order.action.currency},
            {"wallet", order.action.wallet},
            {"created_at", nowIso()},
            {"updated_at", nowIso()},
        };
        auto [status, result] = store.open(position, *mintable, *balance);
        reply(res, status, result);
    });

    server.Get("/api/v1/trade/orders", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string wallet = req.get_param_value("wallet");
        if (!isWallet(wallet)) {
            reply(res, 400, {{"error", "Paramètre wallet (0x + 40 caractères) requis"}});
            return;
        }
        reply(res, 200, {{"orders", store.byWallet(wallet)}});
    });

    server.Get(R"(/api/v1/trade/orders/([A-Za-z0-9_]+))", [&](const httplib::Request& req, httplib::Response& res) {
        if (auto position = store.find(req.matches[1])) reply(res, 200, *position);
        else reply(res, 404, {{"error", "Position introuvable"}});
    });

    // Surveillance : stop loss, plafond de gain, versements en attente
    std::atomic<bool> running{true};
    std::thread monitor([&] {
        while (running) {
            if (auto price = settler.price(); price && *price > 0) {
                for (const auto& id : store.idsWithStatus("open")) {
                    auto position = store.find(id);
                    if (!position) continue;
                    const auto reason = vyfttrade::checkClose(toPosition(*position), *price);
                    if (reason == vyfttrade::CloseReason::None) continue;
                    auto [status, closing] = store.beginClose(id, "", "open");
                    if (status == 200) closePosition(store, settler, closing, *price, reason);
                }
            }
            for (const auto& id : store.idsWithStatus("payout_pending")) {
                auto [status, closing] = store.beginClose(id, "", "payout_pending");
                if (status == 200) settle(store, settler, closing);
            }
            for (int i = 0; i < pollSeconds && running; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    std::cout << "Vyft Trade API sur le port " << port << " (positions : " << dataFile.string() << ")" << std::endl;
    const bool listened = server.listen("0.0.0.0", port);
    running = false;
    monitor.join();
    if (!listened) {
        std::cerr << "Impossible d'écouter sur le port " << port << std::endl;
        return 1;
    }
    return 0;
}

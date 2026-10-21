// VyftTrade_server.cpp : API Vyft Trade™ (vyft-trade.vyft-one.com) — positions VEZ à effet de levier
//
//   POST /api/v1/trade                         execute_order : ouvre une position (mise, levier, stop loss)
//                                              cancel_order  : clôt la position orderid au prix courant
//   POST /api/v1/trade/payout                  {orderid, wallet, txhash} : déclare le versement du gain
//                                              (redeemvez envoyé par l'app avec le SDK), vérifié sur Slura
//   GET  /api/v1/trade/quote?wallet=0x…&stake=N prix, offre du pool, frais actuels, levier proposé
//   GET  /api/v1/trade/orders?wallet=0x…       positions d'un sous-wallet VEZ (plus récentes en premier)
//   GET  /api/v1/trade/orders/<orderid>        une position
//   GET  /api/v1/market/pairs[?all=1]          paires listées (et délistées avec all=1)
//   GET  /api/v1/market/pairs/<BASE>-<QUOTE>   une paire
//   GET  /api/v1/market/assets                 actifs (jeton, décimales) et cours en EUR et en VEZ
//   GET  /api/v1/market/prices                 cours de tous les actifs, VEZ compris
//   GET  /api/v1/market/tickers                tickers de toutes les paires (sentiment compris)
//   GET  /api/v1/market/pairs/<B>-<Q>/ticker|book|trades|candles   données d'une paire
//   POST /api/v1/market/orders                 ordre { pair, side, type, amount, price?, account, baseWallet, quoteWallet }
//   DELETE /api/v1/market/orders/<id>?account=  annulation
//   GET  /api/v1/market/orders?account=0x…     ordres d'un client
//   GET  /api/v1/market/trades?account=0x…     échanges d'un client et action à faire (livrer / payer)
//   POST /api/v1/market/trades/<id>/delivery   { account, op } livraison du vendeur (opération SDK)
//   POST /api/v1/market/trades/<id>/payment    { account, ops } paiement de l'acheteur (opérations SDK)
//   POST /api/v1/admin/pairs                   listing (X-ADMIN-KEY)
//   DELETE /api/v1/admin/pairs/<BASE>-<QUOTE>  délisting (X-ADMIN-KEY)
//   GET  /health                               état du service
//
// Lancement : VyftTradeServer [port=1542] [fichier=orders.json]
//   VYFTTRADE_API_KEY       facultatif : exige l'en-tête X-API-KEY sur /api
//   VYFTTRADE_ADMIN_KEY     clé d'administration (listing) ; sans elle, l'administration est fermée
//   VYFTTRADE_PAIRS_FILE    paires listées (défaut : pairs.json à côté du fichier des positions)
//   VYFTTRADE_POLL_SECONDS  intervalle de surveillance des stop loss (défaut 15)
//   VYFTTRADE_POOL_SEED     VEZ du pool VyftHealth alloués au trading par Vyft (défaut 0 : trading fermé)
//   VYFTTRADE_POOL_ADDRESS  contrat VyftHealth (défaut 0x352aBa76dDC0c4002134333417543fbb268b21a1)
//   VYFT_RPC_URL            RPC Slura (défaut https://slu-charene.vyft-one.com)
//   POR_URL                 preuve de réserves (défaut https://por.vyft-one.com/api/mint)
// Le TLS est assuré par le proxy frontal (vyft-trade.vyft-one.com → ce service en HTTP).
//
// Règles (voir VyftTrade_motor.h) : la mise reste sur le wallet du client ; au stop loss la position
// est fermée sans perte ni gain ; un gain est versé depuis le pool VyftHealth, diminué de frais
// dynamiques qui restent dans le pool. Le serveur ne détient aucune clé : il calcule le montant net
// (statut « payout_due »), l'app le verse avec le SDK vyfttxl_mate (redeemvez), puis déclare la
// transaction, que le serveur vérifie (événement vezRedeemed vers le bon wallet, montant suffisant).
//
// Liquidité de trading : compte séparé du solde du pool (qui contient aussi les récompenses de pas).
//   liquidité = VYFTTRADE_POOL_SEED − gains nets dus ou versés ; les positions ouvertes y réservent
//   leur versement net maximum. Le trading ne peut jamais engager plus que cette liquidité, ni plus que
//   le solde réel du pool : les récompenses de pas ne servent jamais à payer un trader.

#include "VyftTrade_book.h"
#include "VyftTrade_market.h"
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

bool isHex(const std::string& s, size_t digits) {
    return s.size() == digits + 2 && s.rfind("0x", 0) == 0 &&
           std::all_of(s.begin() + 2, s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

bool isWallet(const std::string& w) { return isHex(w, 40); }
bool isTxHash(const std::string& h) { return isHex(h, 64); }

// Mot ABI (uint256 en hexadécimal) → nombre ; précision suffisante pour des montants VEZ
long double hexToNumber(const std::string& hex) {
    long double value = 0;
    for (size_t i = hex.rfind("0x", 0) == 0 ? 2 : 0; i < hex.size(); ++i) {
        const int c = std::tolower(static_cast<unsigned char>(hex[i]));
        const int d = std::isdigit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
        if (d < 0) break;
        value = value * 16 + d;
    }
    return value;
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

// ── Lectures sur Slura et la preuve de réserves (aucune clé, lecture seule) ──
// Sélecteurs : keccak256(signature)[0:4] ; événement vezRedeemed(address,uint256,address,bytes32)
constexpr const char* kSelVezToken = "0x07eb54a9";          // vezToken()
constexpr const char* kSelDecimals = "0x313ce567";          // decimals()
constexpr const char* kSelSoldeVyftHealth = "0x48e6f7f0";   // soldeVyftHealth()
constexpr const char* kSelMaxTransfer = "0xfc1b3113";       // MAX_TRANSFER_AMOUNT()
constexpr const char* kSelEarnRatio = "0x314465d2";         // earnRatio()
constexpr const char* kSelWalletBalance = "0x329a27e7";     // getWalletBalance() du sous-wallet VEZ
constexpr const char* kSelDisburse = "0x26d79ce5";          // disburse(address,uint256,string)
constexpr const char* kTopicVezRedeemed = "0x8c7c3a79ccdd78795909c9ee121301223db0f4025354bb7f4c6396f99a47b249";

struct PoolInfo {
    double balance = 0;       // VEZ du pool
    double maxTransfer = 0;   // plafond d'un versement (VEZ)
    double earnRatio = 1;     // VEZ versés = valeur demandée × earnRatio
    int decimals = 18;        // décimales du jeton VEZ
};

enum class PayoutCheck { Valid, Pending, Invalid };

class Chain {
public:
    Chain(std::string rpcUrl, std::string porUrl, std::string pool)
        : rpcUrl_(std::move(rpcUrl)), porUrl_(std::move(porUrl)), pool_(lower(std::move(pool))) {}

    const std::string& pool() const { return pool_; }

    // Prix d'1 VEZ en EUR : réserve EUR ÷ offre (ou prix direct s'il est publié)
    std::optional<double> price() {
        const auto res = vyfttrade::httpGet(porUrl_);
        if (!res.ok) return std::nullopt;
        try {
            const json data = json::parse(res.body);
            const double direct = number(data, "priceEUR", number(data, "price"));
            const double reserve = number(data, "reserveEUR");
            const double supply = number(data, "totalSupply");
            const double value = direct > 0 ? direct : (reserve > 0 && supply > 0 ? reserve / supply : 0);
            if (value > 0 && std::isfinite(value)) return value;
        } catch (...) {
        }
        return std::nullopt;
    }

    // Réglages et solde du pool VyftHealth, lus sur la chaîne (décimales du jeton comprises)
    std::optional<PoolInfo> poolInfo() {
        const int dec = decimals();
        auto balance = call(pool_, kSelSoldeVyftHealth);
        auto maxTransfer = call(pool_, kSelMaxTransfer);
        auto ratio = call(pool_, kSelEarnRatio);
        if (!balance || !maxTransfer || !ratio) return std::nullopt;
        PoolInfo info;
        info.decimals = dec;
        const long double unit = std::pow(10.0L, dec);
        info.balance = static_cast<double>(hexToNumber(*balance) / unit);
        info.maxTransfer = static_cast<double>(hexToNumber(*maxTransfer) / unit);
        info.earnRatio = static_cast<double>(hexToNumber(*ratio) / 1e6L);
        return info;
    }

    // Solde VEZ d'un sous-wallet (getWalletBalance, comme l'app)
    std::optional<double> walletBalance(const std::string& wallet) {
        auto value = call(wallet, kSelWalletBalance);
        if (!value) return std::nullopt;
        return static_cast<double>(hexToNumber(*value) / std::pow(10.0L, decimals()));
    }

    // Vérifie qu'une transaction a versé au moins `minVez` VEZ du pool vers `to` (événement vezRedeemed)
    PayoutCheck verifyRedeem(const std::string& txHash, const std::string& to, double minVez, std::string& error) {
        auto receipt = rpc("eth_getTransactionReceipt", json::array({txHash}));
        if (!receipt) {
            error = "RPC Slura injoignable";
            return PayoutCheck::Pending;
        }
        if (receipt->is_null()) {
            error = "Transaction pas encore incluse dans un bloc";
            return PayoutCheck::Pending;
        }
        if (receipt->value("status", "") != "0x1") {
            error = "Transaction revertée";
            return PayoutCheck::Invalid;
        }
        const std::string topicTo = "0x000000000000000000000000" + lower(to).substr(2);
        const long double unit = std::pow(10.0L, decimals());
        for (const auto& log : receipt->value("logs", json::array())) {
            const json topics = log.value("topics", json::array());
            if (lower(log.value("address", "")) != pool_ || topics.size() < 2) continue;
            if (!topics[0].is_string() || !topics[1].is_string()) continue;
            if (lower(topics[0].get<std::string>()) != kTopicVezRedeemed) continue;
            if (lower(topics[1].get<std::string>()) != topicTo) continue;
            const std::string data = log.value("data", "");
            const long double paid = hexToNumber(data.substr(0, std::min<size_t>(data.size(), 66))) / unit;
            // Tolérance d'une unité VEZ (6 décimales) pour l'arrondi de earnRatio
            if (paid + 1e-6L >= static_cast<long double>(minVez)) return PayoutCheck::Valid;
            error = "Montant versé insuffisant";
            return PayoutCheck::Invalid;
        }
        error = "Aucun versement vezRedeemed du pool vers ce wallet dans cette transaction";
        return PayoutCheck::Invalid;
    }

    // Solde exact d'un wallet VyftVEZ (un jeton par wallet), en unités du jeton
    std::optional<vyfttrade::U512> walletUnits(const std::string& wallet) {
        auto value = call(wallet, kSelWalletBalance);
        if (!value) return std::nullopt;
        return vyfttrade::U512::parseHex(value->substr(0, std::min<size_t>(value->size(), 66)));
    }

    // Opération SDK (UserOperation) : disburse(to, value, clé) exécuté par le wallet `sender`
    vyfttrade::VerifiedTransfer verifyDisburse(const std::string& userOpHash) {
        using State = vyfttrade::VerifiedTransfer::State;
        vyfttrade::VerifiedTransfer out;
        auto op = rpc("eth_getUserOperationByHash", json::array({userOpHash}));
        if (!op) {
            out.state = State::Pending;
            out.error = "RPC Slura injoignable";
            return out;
        }
        if (op->is_null()) {
            out.state = State::Pending;
            out.error = "Opération inconnue ou pas encore incluse dans un bloc";
            return out;
        }
        const json userOp = op->value("userOperation", json::object());
        const std::string callData = lower(userOp.value("callData", ""));
        out.sender = lower(userOp.value("sender", ""));
        // disburse(address,uint256,string) : sélecteur 0x26d79ce5, destinataire et montant dans les mots 1 et 2
        if (callData.rfind(kSelDisburse, 0) != 0 || callData.size() < 10 + 64 * 2) {
            out.error = "Cette opération n'est pas un envoi (disburse) du wallet";
            return out;
        }
        out.to = "0x" + callData.substr(10 + 24, 40);
        out.value = vyfttrade::U512::parseHex(callData.substr(10 + 64, 64)).value_or(vyfttrade::U512());

        auto receipt = rpc("eth_getUserOperationReceipt", json::array({userOpHash}));
        if (!receipt || receipt->is_null()) {
            out.state = State::Pending;
            out.error = "Opération pas encore exécutée";
            return out;
        }
        const json& success = receipt->contains("success") ? receipt->at("success") : json(nullptr);
        const bool ok = success.is_boolean() ? success.get<bool>() : success == "0x1";
        out.state = ok ? State::Ok : State::Failed;
        if (!ok) out.error = "Envoi refusé par le wallet (solde ou clé VyftID)";
        return out;
    }

    // Un contrat est-il déployé à cette adresse ? (listing d'un jeton)
    bool hasCode(const std::string& address) {
        auto code = rpc("eth_getCode", json::array({address, "latest"}));
        return code && code->is_string() && code->get<std::string>().size() > 2;
    }

    // Décimales d'un jeton (decimals()) ; nullopt si illisibles
    std::optional<int> tokenDecimals(const std::string& token) {
        auto dec = call(token, kSelDecimals);
        if (!dec) return std::nullopt;
        const long double value = hexToNumber(*dec);
        if (value < 0 || value > 36) return std::nullopt;
        return static_cast<int>(value);
    }

private:
    std::optional<json> rpc(const std::string& method, const json& params) {
        const json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}};
        const auto res = vyfttrade::httpPostJson(rpcUrl_, request.dump());
        if (!res.ok) return std::nullopt;
        try {
            const json body = json::parse(res.body);
            if (body.contains("error") || !body.contains("result")) return std::nullopt;
            return body.at("result");
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<std::string> call(const std::string& to, const char* data) {
        const json callObject = {{"to", to}, {"data", data}};
        auto result = rpc("eth_call", json::array({callObject, "latest"}));
        if (!result || !result->is_string()) return std::nullopt;
        const std::string hex = result->get<std::string>();
        if (hex.size() < 3) return std::nullopt;
        return hex;
    }

    // Décimales du jeton VEZ du pool (lues une fois, 18 à défaut)
    int decimals() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (decimals_ > 0) return decimals_;
        if (auto token = call(pool_, kSelVezToken); token && token->size() >= 42) {
            if (auto dec = call("0x" + token->substr(token->size() - 40), kSelDecimals)) {
                const int value = static_cast<int>(hexToNumber(*dec));
                if (value > 0 && value <= 36) return decimals_ = value;
            }
        }
        return 18;
    }

    std::string rpcUrl_;
    std::string porUrl_;
    std::string pool_;
    int decimals_ = 0;
    std::mutex mutex_;
};

// Versement net maximum réservé par une position (anciennes positions : gain brut maximum)
double reservedPayout(const json& p) { return number(p, "maxpayout", number(p, "maxgain")); }

// État de la liquidité de trading
struct PoolState {
    double seed = 0;        // VEZ alloués au trading
    double spent = 0;       // gains nets dus ou versés
    double liquidity = 0;   // seed − spent
    double reserved = 0;    // versements nets maximum des positions ouvertes
};

// ── Positions persistantes (fichier JSON réécrit à chaque modification) ──
class PositionStore {
public:
    PositionStore(std::filesystem::path file, double seed) : file_(std::move(file)), seed_(seed) {
        std::ifstream in(file_);
        if (in) {
            try {
                in >> items_;
            } catch (...) {
                items_ = json::array();
            }
        }
        if (!items_.is_array()) items_ = json::array();
        // Clôture interrompue par un arrêt : chiffrée → versement dû ; pas encore chiffrée → rouverte
        for (auto& p : items_)
            if (p.value("status", "") == "closing") p["status"] = p.contains("payout") ? "payout_due" : "open";
    }

    PoolState pool() {
        std::lock_guard<std::mutex> lock(mutex_);
        return poolLocked();
    }

    // Offre encore engageable : bornée par le compte de trading et par le solde réel du pool
    double available(double poolBalance) {
        std::lock_guard<std::mutex> lock(mutex_);
        return availableLocked(poolBalance);
    }

    // Taux de frais actuel ; `excludeId` retire la réservation de la position qui se clôt
    double feeRate(const std::string& excludeId = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        PoolState s = poolLocked();
        if (!excludeId.empty())
            for (const auto& p : items_)
                if (p.value("orderid", "") == excludeId && reservesOpen(p)) s.reserved -= reservedPayout(p);
        const double health = s.seed > 0 ? s.liquidity / s.seed : 0;
        const double utilization = s.liquidity > 0 ? std::max(0.0, s.reserved) / s.liquidity : 1;
        return vyfttrade::profitFeeRate(health, utilization);
    }

    double openStake(const std::string& wallet) {
        std::lock_guard<std::mutex> lock(mutex_);
        return openStakeLocked(wallet);
    }

    // Ouvre la position si l'offre du pool et le solde du client la couvrent encore (sous verrou)
    std::pair<int, json> open(json position, const PoolInfo& info, double walletBalance) {
        std::lock_guard<std::mutex> lock(mutex_);
        const vyfttrade::Position pos = toPosition(position);
        const double available = availableLocked(info.balance);
        if (auto error = vyfttrade::validatePosition(pos, available, info.maxTransfer))
            return {422, {{"error", *error}}};
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

    // Réserve une transaction de versement pour une position due (un hash ne sert qu'une fois)
    std::pair<int, json> claimPayout(const std::string& orderId, const std::string& wallet, const std::string& txHash) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& p : items_)
            if (lower(p.value("txhash", "")) == lower(txHash) && p.value("orderid", "") != orderId)
                return {409, {{"error", "Cette transaction a déjà servi à une autre position"}}};
        for (auto& p : items_) {
            if (p.value("orderid", "") != orderId) continue;
            if (lower(p.value("wallet", "")) != lower(wallet))
                return {403, {{"error", "Cette position n'appartient pas à ce sous-wallet"}}};
            if (p.value("status", "") == "closed") return {200, p};
            if (p.value("status", "") != "payout_due")
                return {409, {{"error", "Aucun versement dû pour cette position (" + p.value("status", "") + ")"}}};
            // Hash réservé dès maintenant : une déclaration simultanée ne peut pas le réutiliser
            p["txhash"] = lower(txHash);
            p["updated_at"] = nowIso();
            save();
            return {202, p};
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
    // Position qui réserve encore son versement maximum (ouverte, ou clôture pas encore chiffrée)
    static bool reservesOpen(const json& p) {
        const std::string status = p.value("status", "");
        return status == "open" || (status == "closing" && !p.contains("payout"));
    }

    PoolState poolLocked() const {
        PoolState s;
        s.seed = seed_;
        for (const auto& p : items_) {
            if (reservesOpen(p)) s.reserved += reservedPayout(p);
            else s.spent += number(p, "payout");   // dû (payout_due) ou versé (closed)
        }
        s.liquidity = s.seed - s.spent;
        return s;
    }

    double availableLocked(double poolBalance) const {
        const PoolState s = poolLocked();
        return std::max(0.0, std::min(s.liquidity, poolBalance) - s.reserved);
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
    double seed_ = 0;
    json items_ = json::array();
    std::mutex mutex_;
};

// Clôture : gain brut au prix courant, frais dynamiques (restent dans le pool), net dû au client
json closePosition(PositionStore& store, json position, double price, vyfttrade::CloseReason reason) {
    const std::string id = position.value("orderid", "");
    const vyfttrade::Position pos = toPosition(position);
    const double gross = vyfttrade::settlementAmount(pos, price, reason);
    const vyfttrade::ProfitSplit split = vyfttrade::splitProfit(gross, gross > 0 ? store.feeRate(id) : 0);
    return store.update(id, [&](json& p) {
        p["closeprice"] = decimal(price);
        p["closereason"] = vyfttrade::toString(reason);
        p["closed_at"] = nowIso();
        p["gross"] = vyfttrade::formatQuantity(split.gross);
        p["feerate"] = decimal(split.feeRate, 4);
        p["fee"] = vyfttrade::formatQuantity(split.fee);
        p["payout"] = vyfttrade::formatQuantity(split.net);
        if (split.net > 0) {
            p["status"] = "payout_due";   // l'app verse le net avec le SDK, puis le déclare
        } else {
            p["status"] = "closed";       // stop loss ou position sans gain : rien n'est perdu ni versé
            p["result"] = "nul";
        }
    });
}

void reply(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

}  // namespace

int main(int argc, char** argv) {
    const int port = argc > 1 ? std::atoi(argv[1]) : 1542;
    const std::filesystem::path dataFile = argc > 2 ? argv[2] : "orders.json";
    auto envOr = [](const char* name, const char* fallback) {
        const char* v = std::getenv(name);
        return std::string(v && *v ? v : fallback);
    };
    const std::string apiKey = envOr("VYFTTRADE_API_KEY", "");
    const int pollSeconds = std::max(5, std::atoi(envOr("VYFTTRADE_POLL_SECONDS", "15").c_str()));

    const double poolSeed = std::max(0.0, std::strtod(envOr("VYFTTRADE_POOL_SEED", "0").c_str(), nullptr));
    PositionStore store(dataFile, std::isfinite(poolSeed) ? poolSeed : 0);
    Chain chain(envOr("VYFT_RPC_URL", "https://slu-charene.vyft-one.com"),
                envOr("POR_URL", "https://por.vyft-one.com/api/mint"),
                envOr("VYFTTRADE_POOL_ADDRESS", "0x352aBa76dDC0c4002134333417543fbb268b21a1"));

    // Paires listées : à côté du fichier des positions (pairs.json), ou VYFTTRADE_PAIRS_FILE
    const std::string adminKey = envOr("VYFTTRADE_ADMIN_KEY", "");
    const std::filesystem::path pairsFile =
        envOr("VYFTTRADE_PAIRS_FILE", (dataFile.parent_path() / "pairs.json").string().c_str());
    vyfttrade::MarketRegistry registry(pairsFile);
    vyfttrade::PriceBook priceBook([](const std::string& url) -> std::optional<std::string> {
        const auto res = vyfttrade::httpGet(url);
        return res.ok ? std::optional<std::string>(res.body) : std::nullopt;
    });

    // Carnets d'ordres : à côté du fichier des positions (book.json), ou VYFTTRADE_BOOK_FILE.
    // Frais du vendeur (VYFTTRADE_TRADE_FEE_PPM, défaut 2000 = 0,2 %) versés au pool VyftHealth.
    vyfttrade::BookConfig bookConfig;
    bookConfig.walletUnits = [&](const std::string& wallet) { return chain.walletUnits(wallet); };
    bookConfig.feeRecipient = chain.pool();
    bookConfig.feeRatePpm = std::strtoull(envOr("VYFTTRADE_TRADE_FEE_PPM", "2000").c_str(), nullptr, 10);
    bookConfig.deliverySeconds = std::max(60, std::atoi(envOr("VYFTTRADE_DELIVERY_SECONDS", "600").c_str()));
    bookConfig.paymentSeconds = std::max(60, std::atoi(envOr("VYFTTRADE_PAYMENT_SECONDS", "900").c_str()));
    vyfttrade::OrderBook book(envOr("VYFTTRADE_BOOK_FILE", (dataFile.parent_path() / "book.json").string().c_str()),
                              bookConfig);

    // Cours de référence d'une paire en unités de cotation (VEZ) : source externe convertie via la preuve de réserves
    auto indexInQuote = [&](const vyfttrade::Pair& p) -> std::optional<double> {
        if (!p.index || p.quote.symbol != "VEZ") return std::nullopt;
        auto value = priceBook.indexValue(*p.index);
        if (!value) return std::nullopt;
        if (p.index->currency == "VEZ") return value;
        auto vezEur = chain.price();
        if (!vezEur || *vezEur <= 0) return std::nullopt;
        return *value / *vezEur;
    };
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

    // ── Marché : paires listées et cours de tous les actifs (VEZ compris) ──
    server.Get("/api/v1/market/pairs", [&](const httplib::Request& req, httplib::Response& res) {
        json pairs = json::array();
        for (const auto& p : registry.pairs(req.get_param_value("all") == "1")) pairs.push_back(vyfttrade::toJson(p));
        reply(res, 200, {{"pairs", pairs}});
    });

    server.Get(R"(/api/v1/market/pairs/([A-Za-z0-9]+)[-_]([A-Za-z0-9]+))", [&](const httplib::Request& req, httplib::Response& res) {
        if (auto p = registry.find(std::string(req.matches[1]) + "/" + std::string(req.matches[2]))) reply(res, 200, vyfttrade::toJson(*p));
        else reply(res, 404, {{"error", "Paire introuvable"}});
    });

    // Actifs à proposer dans l'app (jeton et décimales pour créer le wallet de chaque actif) et leur cours
    server.Get("/api/v1/market/assets", [&](const httplib::Request&, httplib::Response& res) {
        const json prices = priceBook.prices(registry, chain.price());
        json assets = json::array();
        auto add = [&](const vyfttrade::Asset& a, const std::string& status) {
            for (const auto& existing : assets)
                if (existing.value("symbol", "") == a.symbol) return;
            json entry = vyfttrade::toJson(a);
            entry["status"] = status;
            if (prices.contains(a.symbol)) {
                entry["eur"] = prices.at(a.symbol).value("eur", json(nullptr));
                entry["vez"] = prices.at(a.symbol).value("vez", json(nullptr));
            }
            assets.push_back(entry);
        };
        add(vyfttrade::kVezAssetInfo, "listed");
        for (const auto& p : registry.pairs(true)) add(p.base, p.status);
        reply(res, 200, {{"assets", assets}});
    });

    server.Get("/api/v1/market/prices", [&](const httplib::Request&, httplib::Response& res) {
        reply(res, 200, {{"prices", priceBook.prices(registry, chain.price())}, {"updated_at", nowIso()}});
    });

    // ── Administration : listing et délisting (X-ADMIN-KEY = VYFTTRADE_ADMIN_KEY) ──
    auto adminAllowed = [&](const httplib::Request& req, httplib::Response& res) {
        if (adminKey.empty()) {
            reply(res, 503, {{"error", "Administration désactivée : définissez VYFTTRADE_ADMIN_KEY"}});
            return false;
        }
        if (req.get_header_value("X-ADMIN-KEY") != adminKey) {
            reply(res, 401, {{"error", "Clé d'administration invalide"}});
            return false;
        }
        return true;
    };

    server.Post("/api/v1/admin/pairs", [&](const httplib::Request& req, httplib::Response& res) {
        if (!adminAllowed(req, res)) return;
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            reply(res, 400, {{"error", "Corps JSON invalide"}});
            return;
        }
        vyfttrade::TokenProbe probe{[&](const std::string& t) { return chain.hasCode(t); },
                                    [&](const std::string& t) { return chain.tokenDecimals(t); }};
        auto [status, result] = registry.list(body, probe);
        reply(res, status, result);
    });

    server.Delete(R"(/api/v1/admin/pairs/([A-Za-z0-9]+)[-_]([A-Za-z0-9]+))", [&](const httplib::Request& req, httplib::Response& res) {
        if (!adminAllowed(req, res)) return;
        auto [status, result] = registry.delist(std::string(req.matches[1]) + "/" + std::string(req.matches[2]));
        // Délisting : les ordres en attente sont annulés ; les échanges en cours se règlent normalement
        if (status == 200) book.cancelPair(result.value("symbol", ""), "delisted");
        reply(res, status, result);
    });

    // ── Carnet d'ordres ──
    auto pairFromPath = [&](const httplib::Request& req) {
        return registry.find(std::string(req.matches[1]) + "/" + std::string(req.matches[2]));
    };
    auto parseBody = [](const httplib::Request& req, json& out) {
        try {
            out = json::parse(req.body);
            return out.is_object();
        } catch (...) {
            return false;
        }
    };

    server.Get("/api/v1/market/tickers", [&](const httplib::Request&, httplib::Response& res) {
        json tickers = json::array();
        for (const auto& p : registry.pairs()) tickers.push_back(book.ticker(p, indexInQuote(p)));
        reply(res, 200, {{"tickers", tickers}});
    });

    server.Get(R"(/api/v1/market/pairs/([A-Za-z0-9]+)[-_]([A-Za-z0-9]+)/(ticker|book|trades|candles))",
               [&](const httplib::Request& req, httplib::Response& res) {
        auto pair = pairFromPath(req);
        if (!pair) {
            reply(res, 404, {{"error", "Paire introuvable"}});
            return;
        }
        const std::string what = req.matches[3];
        if (what == "ticker") {
            reply(res, 200, book.ticker(*pair, indexInQuote(*pair)));
        } else if (what == "book") {
            const size_t depth = std::clamp(std::atoi(req.get_param_value("depth").c_str()), 1, 100);
            reply(res, 200, book.depth(*pair, req.has_param("depth") ? depth : 20));
        } else if (what == "trades") {
            const size_t limit = std::clamp(std::atoi(req.get_param_value("limit").c_str()), 1, 200);
            reply(res, 200, {{"trades", book.recentTrades(*pair, req.has_param("limit") ? limit : 50)}});
        } else {
            static const std::map<std::string, int64_t> intervals = {{"1m", 60}, {"5m", 300}, {"15m", 900}, {"1h", 3600}, {"4h", 14400}, {"1d", 86400}};
            const std::string interval = req.has_param("interval") ? req.get_param_value("interval") : "1h";
            auto it = intervals.find(interval);
            if (it == intervals.end()) {
                reply(res, 400, {{"error", "interval : 1m, 5m, 15m, 1h, 4h ou 1d"}});
                return;
            }
            const size_t limit = std::clamp(std::atoi(req.get_param_value("limit").c_str()), 1, 500);
            reply(res, 200, {{"interval", interval}, {"candles", book.candles(*pair, it->second, req.has_param("limit") ? limit : 100)}});
        }
    });

    // Ordre : { pair, side: buy|sell, type: limit|market, amount, price?, account, baseWallet, quoteWallet }
    server.Post("/api/v1/market/orders", [&](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, body)) {
            reply(res, 400, {{"error", "Corps JSON invalide"}});
            return;
        }
        auto pair = registry.find(body.value("pair", ""));
        if (!pair) {
            reply(res, 404, {{"error", "Paire introuvable"}});
            return;
        }
        auto [status, result] = book.place(*pair, body);
        reply(res, status, result);
    });

    server.Delete(R"(/api/v1/market/orders/([A-Za-z0-9_]+))", [&](const httplib::Request& req, httplib::Response& res) {
        auto [status, result] = book.cancel(req.matches[1], req.get_param_value("account"));
        reply(res, status, result);
    });

    server.Get("/api/v1/market/orders", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string account = req.get_param_value("account");
        if (!isWallet(account)) {
            reply(res, 400, {{"error", "Paramètre account (sous-wallet VEZ) requis"}});
            return;
        }
        reply(res, 200, {{"orders", book.orders(account)}});
    });

    // Échanges du client, avec l'action que son app doit faire (livrer ou payer) et les montants exacts
    server.Get("/api/v1/market/trades", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string account = req.get_param_value("account");
        if (!isWallet(account)) {
            reply(res, 400, {{"error", "Paramètre account (sous-wallet VEZ) requis"}});
            return;
        }
        json out = {{"trades", book.trades(account)}};
        if (auto reason = book.suspension(account)) out["suspended"] = *reason;
        reply(res, 200, out);
    });

    // Livraison du vendeur : { account, op } — op = hash de l'opération SDK (disburse de la base)
    server.Post(R"(/api/v1/market/trades/([A-Za-z0-9_]+)/delivery)", [&](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, body)) {
            reply(res, 400, {{"error", "Corps JSON invalide"}});
            return;
        }
        const std::string op = lower(body.value("op", ""));
        if (!isTxHash(op)) {
            reply(res, 400, {{"error", "op : hash de l'opération SDK (0x + 64 caractères) requis"}});
            return;
        }
        auto [status, result] = book.deliver(req.matches[1], body.value("account", ""), op, chain.verifyDisburse(op));
        reply(res, status, result);
    });

    // Paiement de l'acheteur : { account, ops: [hash, …] } — prix moins frais au vendeur, frais au pool
    server.Post(R"(/api/v1/market/trades/([A-Za-z0-9_]+)/payment)", [&](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, body)) {
            reply(res, 400, {{"error", "Corps JSON invalide"}});
            return;
        }
        std::vector<std::pair<std::string, vyfttrade::VerifiedTransfer>> ops;
        for (const auto& item : body.value("ops", json::array())) {
            const std::string op = item.is_string() ? lower(item.get<std::string>()) : "";
            if (!isTxHash(op)) {
                reply(res, 400, {{"error", "ops : liste de hash d'opérations SDK (0x + 64 caractères)"}});
                return;
            }
            ops.emplace_back(op, chain.verifyDisburse(op));
        }
        if (ops.empty() || ops.size() > 4) {
            reply(res, 400, {{"error", "ops : 1 à 4 opérations"}});
            return;
        }
        auto [status, result] = book.pay(req.matches[1], body.value("account", ""), ops);
        reply(res, status, result);
    });

    // Prix, offre du pool, frais actuels et levier maximum proposé pour une mise
    server.Get("/api/v1/trade/quote", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string wallet = req.get_param_value("wallet");
        const double stake = std::strtod(req.get_param_value("stake").c_str(), nullptr);
        auto price = chain.price();
        if (!price) {
            reply(res, 503, {{"error", "Prix VEZ indisponible (preuve de réserves)"}});
            return;
        }
        const PoolInfo info = chain.poolInfo().value_or(PoolInfo{});
        const PoolState pool = store.pool();
        const double available = store.available(info.balance);
        const double maxLev = vyfttrade::maxLeverage(stake, available, info.maxTransfer);
        json quote = {
            {"price", decimal(*price)},
            {"pool", chain.pool()},
            {"poolbalance", vyfttrade::formatQuantity(info.balance)},
            {"maxtransfer", decimal(info.maxTransfer, 18)},
            {"earnratio", decimal(info.earnRatio, 6)},
            {"decimals", info.decimals},
            {"liquidity", vyfttrade::formatQuantity(pool.liquidity)},
            {"reserved", vyfttrade::formatQuantity(pool.reserved)},
            {"available", vyfttrade::formatQuantity(available)},
            {"feerate", decimal(store.feeRate(), 4)},
            {"feemin", decimal(vyfttrade::kProfitFeeMin, 4)},
            {"feemax", decimal(vyfttrade::kProfitFeeMax, 4)},
            {"gaincapmove", decimal(vyfttrade::kGainCapMove, 4)},
            {"maxleverage", decimal(maxLev, 0)},
            {"suggestedleverage", decimal(maxLev, 0)},
        };
        if (isWallet(wallet)) {
            quote["balance"] = vyfttrade::formatQuantity(chain.walletBalance(wallet).value_or(0));
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

        auto price = chain.price();
        if (!price) {
            reply(res, 503, {{"error", "Prix VEZ indisponible (preuve de réserves)"}});
            return;
        }

        if (*actionType == vyfttrade::ActionType::CancelOrder) {
            auto [status, position] = store.beginClose(order.action.orderId, order.action.wallet, "open");
            if (status != 200) {
                reply(res, status, position);
                return;
            }
            reply(res, 200, closePosition(store, position, *price, vyfttrade::CloseReason::None));
            return;
        }

        auto balance = chain.walletBalance(order.action.wallet);
        auto info = chain.poolInfo();
        if (!balance || !info) {
            reply(res, 503, {{"error", "Solde du wallet ou du pool indisponible (RPC Slura)"}});
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
            {"maxpayout", vyfttrade::formatQuantity(vyfttrade::maxPayout(pos.stake, pos.leverage))},
            {"currency", order.action.currency},
            {"wallet", order.action.wallet},
            {"created_at", nowIso()},
            {"updated_at", nowIso()},
        };
        auto [status, result] = store.open(position, *info, *balance);
        reply(res, status, result);
    });

    // Déclaration du versement d'un gain (redeemvez envoyé par l'app), vérifié sur la chaîne
    server.Post("/api/v1/trade/payout", [&](const httplib::Request& req, httplib::Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            reply(res, 400, {{"error", "Corps JSON invalide"}});
            return;
        }
        const std::string orderId = field(body, "orderid");
        const std::string wallet = field(body, "wallet");
        const std::string txHash = lower(field(body, "txhash"));
        if (orderId.empty() || !isWallet(wallet) || !isTxHash(txHash)) {
            reply(res, 400, {{"error", "orderid, wallet (0x…) et txhash (0x + 64 caractères) requis"}});
            return;
        }
        auto [status, position] = store.claimPayout(orderId, wallet, txHash);
        if (status != 202) {
            reply(res, status, position);
            return;
        }
        std::string error;
        switch (chain.verifyRedeem(txHash, wallet, number(position, "payout"), error)) {
            case PayoutCheck::Valid:
                reply(res, 200, store.update(orderId, [&](json& p) {
                    p["status"] = "closed";
                    p["result"] = "gain";
                    p["txhash"] = txHash;
                    p["paid_at"] = nowIso();
                    p.erase("error");
                }));
                return;
            case PayoutCheck::Pending:
                reply(res, 202, {{"orderid", orderId}, {"status", "payout_due"}, {"error", error}});
                return;
            case PayoutCheck::Invalid:
                store.update(orderId, [](json& p) { p.erase("txhash"); });   // hash libéré : il ne paie rien ici
                reply(res, 422, {{"orderid", orderId}, {"status", "payout_due"}, {"error", error}});
                return;
        }
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

    // Surveillance : stop loss et gain maximum atteints
    std::atomic<bool> running{true};
    std::thread monitor([&] {
        while (running) {
            if (auto price = chain.price()) {
                for (const auto& id : store.idsWithStatus("open")) {
                    auto position = store.find(id);
                    if (!position) continue;
                    const auto reason = vyfttrade::checkClose(toPosition(*position), *price);
                    if (reason == vyfttrade::CloseReason::None) continue;
                    auto [status, closing] = store.beginClose(id, "", "open");
                    if (status == 200) closePosition(store, closing, *price, reason);
                }
            }
            book.expire();   // livraisons et paiements hors délai
            for (int i = 0; i < pollSeconds && running; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    std::cout << "Vyft Trade API sur le port " << port << " (positions : " << dataFile.string() << ", pool "
              << chain.pool() << ")" << std::endl;
    const bool listened = server.listen("0.0.0.0", port);
    running = false;
    monitor.join();
    if (!listened) {
        std::cerr << "Impossible d'écouter sur le port " << port << std::endl;
        return 1;
    }
    return 0;
}

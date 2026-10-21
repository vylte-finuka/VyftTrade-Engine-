// VyftTrade_book.cpp : carnets d'ordres, appariement, règlement vendeur d'abord, données de marché
//
// Un wallet par jeton et par client : un ordre BASE/COTATION engage le wallet BASE du client (vente)
// ou son wallet COTATION (achat). L'engagement d'un wallet = ses ordres ouverts + ses échanges non
// réglés ; le disponible = solde sur la chaîne − engagement.

#include "VyftTrade_book.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace vyfttrade {

using json = nlohmann::json;

namespace {

int64_t nowEpoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string isoFromEpoch(int64_t epoch) {
    const std::time_t t = static_cast<std::time_t>(epoch);
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

std::string nowIso() { return isoFromEpoch(nowEpoch()); }

std::string newId(const char* prefix) {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream out;
    out << prefix << std::hex << std::setfill('0') << std::setw(16) << rng() << std::setw(16) << rng();
    return out.str();
}

std::string lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

bool isAddress(const std::string& a) {
    return a.size() == 42 && a.rfind("0x", 0) == 0 &&
           std::all_of(a.begin() + 2, a.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string text(const json& o, const char* key) {
    if (!o.is_object() || !o.contains(key)) return {};
    const json& v = o.at(key);
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) return v.dump();
    return {};
}

U512 u512(const json& o, const char* key) {
    auto v = U512::parseInteger(text(o, key));
    return v ? *v : U512();
}

// Prix total en unités de cotation : quantité (unités base) × prix (cotation par jeton × 10^18)
U512 quoteOf(const U512& qty, const U512& price, int baseDecimals, int quoteDecimals, bool roundUp = false) {
    return U512::mulDiv(qty, price * U512::pow10(quoteDecimals), U512::pow10(baseDecimals + kPriceDecimals), roundUp);
}

// Quantité de base que `budget` (unités de cotation) permet d'acheter au prix donné
U512 qtyFor(const U512& budget, const U512& price, int baseDecimals, int quoteDecimals) {
    if (price.isZero()) return U512();
    return U512::mulDiv(budget, U512::pow10(baseDecimals + kPriceDecimals), price * U512::pow10(quoteDecimals));
}

bool openStatus(const std::string& s) { return s == "open"; }
bool unsettled(const std::string& s) {
    return s == "awaiting_delivery" || s == "awaiting_payment" || s == "payment_overdue";
}

std::string formatPrice(const U512& price) { return price.formatUnits(kPriceDecimals); }

}  // namespace

OrderBook::OrderBook(std::filesystem::path file, BookConfig config) : file_(std::move(file)), config_(std::move(config)) {
    config_.feeRecipient = lower(config_.feeRecipient);
    load();
}

// ── Persistance ──

void OrderBook::load() {
    std::ifstream in(file_);
    if (!in) return;
    try {
        json data;
        in >> data;
        for (const auto& j : data.value("orders", json::array())) {
            BookOrder o;
            o.id = j.value("id", "");
            o.pair = j.value("pair", "");
            o.side = j.value("side", "");
            o.type = j.value("type", "");
            o.status = j.value("status", "");
            o.account = j.value("account", "");
            o.baseWallet = j.value("baseWallet", "");
            o.quoteWallet = j.value("quoteWallet", "");
            o.baseDecimals = j.value("baseDecimals", 18);
            o.quoteDecimals = j.value("quoteDecimals", 18);
            o.price = u512(j, "priceUnits");
            o.amount = u512(j, "amountUnits");
            o.filled = u512(j, "filledUnits");
            o.reason = j.value("reason", "");
            o.createdAt = j.value("created_at", "");
            o.updatedAt = j.value("updated_at", "");
            o.seq = j.value("seq", int64_t{0});
            orders_.push_back(o);
        }
        for (const auto& j : data.value("trades", json::array())) {
            BookTrade t;
            t.id = j.value("id", "");
            t.pair = j.value("pair", "");
            t.status = j.value("status", "");
            t.takerSide = j.value("takerSide", "");
            t.buyOrder = j.value("buyOrder", "");
            t.sellOrder = j.value("sellOrder", "");
            t.buyer = j.value("buyer", "");
            t.buyerBaseWallet = j.value("buyerBaseWallet", "");
            t.buyerQuoteWallet = j.value("buyerQuoteWallet", "");
            t.seller = j.value("seller", "");
            t.sellerBaseWallet = j.value("sellerBaseWallet", "");
            t.sellerQuoteWallet = j.value("sellerQuoteWallet", "");
            t.baseDecimals = j.value("baseDecimals", 18);
            t.quoteDecimals = j.value("quoteDecimals", 18);
            t.price = u512(j, "priceUnits");
            t.baseAmount = u512(j, "baseUnits");
            t.quoteAmount = u512(j, "quoteUnits");
            t.fee = u512(j, "feeUnits");
            t.feeRecipient = j.value("feeRecipient", "");
            t.deliveryOp = j.value("deliveryOp", "");
            t.paymentOps = j.value("paymentOps", std::vector<std::string>{});
            t.reason = j.value("reason", "");
            t.createdAt = j.value("created_at", "");
            t.createdEpoch = j.value("createdEpoch", int64_t{0});
            t.deliveryDeadline = j.value("deliveryDeadline", int64_t{0});
            t.paymentDeadline = j.value("paymentDeadline", int64_t{0});
            t.deliveredAt = j.value("delivered_at", "");
            t.settledAt = j.value("settled_at", "");
            trades_.push_back(t);
        }
        for (const auto& op : data.value("usedOps", json::array())) usedOps_.insert(op.get<std::string>());
        seq_ = data.value("seq", int64_t{0});
    } catch (...) {
        orders_.clear();
        trades_.clear();
        usedOps_.clear();
    }
}

void OrderBook::save() {
    json data = {{"orders", json::array()}, {"trades", json::array()}, {"usedOps", usedOps_}, {"seq", seq_}};
    for (const auto& o : orders_) {
        data["orders"].push_back({
            {"id", o.id}, {"pair", o.pair}, {"side", o.side}, {"type", o.type}, {"status", o.status},
            {"account", o.account}, {"baseWallet", o.baseWallet}, {"quoteWallet", o.quoteWallet},
            {"baseDecimals", o.baseDecimals}, {"quoteDecimals", o.quoteDecimals},
            {"priceUnits", o.price.toDecimalString()}, {"amountUnits", o.amount.toDecimalString()},
            {"filledUnits", o.filled.toDecimalString()}, {"reason", o.reason}, {"created_at", o.createdAt},
            {"updated_at", o.updatedAt}, {"seq", o.seq},
        });
    }
    for (const auto& t : trades_) {
        data["trades"].push_back({
            {"id", t.id}, {"pair", t.pair}, {"status", t.status}, {"takerSide", t.takerSide},
            {"buyOrder", t.buyOrder}, {"sellOrder", t.sellOrder}, {"buyer", t.buyer},
            {"buyerBaseWallet", t.buyerBaseWallet}, {"buyerQuoteWallet", t.buyerQuoteWallet}, {"seller", t.seller},
            {"sellerBaseWallet", t.sellerBaseWallet}, {"sellerQuoteWallet", t.sellerQuoteWallet},
            {"baseDecimals", t.baseDecimals}, {"quoteDecimals", t.quoteDecimals},
            {"priceUnits", t.price.toDecimalString()}, {"baseUnits", t.baseAmount.toDecimalString()},
            {"quoteUnits", t.quoteAmount.toDecimalString()}, {"feeUnits", t.fee.toDecimalString()},
            {"feeRecipient", t.feeRecipient}, {"deliveryOp", t.deliveryOp}, {"paymentOps", t.paymentOps},
            {"reason", t.reason}, {"created_at", t.createdAt}, {"createdEpoch", t.createdEpoch},
            {"deliveryDeadline", t.deliveryDeadline}, {"paymentDeadline", t.paymentDeadline},
            {"delivered_at", t.deliveredAt}, {"settled_at", t.settledAt},
        });
    }
    const auto tmp = file_.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << data.dump(2);
    }
    std::filesystem::rename(tmp, file_);
}

// ── Engagement et disponible d'un wallet (un jeton par wallet) ──

U512 OrderBook::committed(const std::string& wallet) const {
    U512 total;
    for (const auto& o : orders_) {
        if (!openStatus(o.status)) continue;
        if (o.side == "sell" && o.baseWallet == wallet) total = total + o.remaining();
        if (o.side == "buy" && o.quoteWallet == wallet)
            total = total + quoteOf(o.remaining(), o.price, o.baseDecimals, o.quoteDecimals, true);
    }
    for (const auto& t : trades_) {
        if (t.status == "awaiting_delivery" && t.sellerBaseWallet == wallet) total = total + t.baseAmount;
        if (unsettled(t.status) && t.buyerQuoteWallet == wallet) total = total + t.quoteAmount;
    }
    return total;
}

std::optional<U512> OrderBook::available(const std::string& wallet, std::map<std::string, std::optional<U512>>& balances) const {
    auto it = balances.find(wallet);
    if (it == balances.end()) it = balances.emplace(wallet, config_.walletUnits(wallet)).first;
    if (!it->second) return std::nullopt;
    const U512 used = committed(wallet);
    return *it->second > used ? *it->second - used : U512();
}

std::optional<std::string> OrderBook::suspensionLocked(const std::string& account) const {
    int missed = 0;
    const int64_t since = nowEpoch() - 86400;
    for (const auto& t : trades_) {
        if (t.buyer == account && t.status == "payment_overdue")
            return "Paiement en retard sur l'échange " + t.id + " : réglez-le pour pouvoir trader à nouveau";
        if (t.seller == account && t.status == "cancelled" && t.reason == "non_delivery" && t.createdEpoch >= since) ++missed;
    }
    if (missed >= 3) return std::string("3 livraisons manquées en 24 h : trading suspendu temporairement");
    return std::nullopt;
}

std::optional<std::string> OrderBook::suspension(const std::string& account) {
    std::lock_guard<std::mutex> lock(mutex_);
    return suspensionLocked(lower(account));
}

// ── Ordres ──

std::pair<int, json> OrderBook::place(const Pair& pair, const json& request) {
    if (pair.status != "listed") return {409, {{"error", "Paire délistée"}}};
    if (pair.quote.token.empty() || pair.base.token.empty())
        return {409, {{"error", pair.symbol() + " est une paire de cours : aucun échange possible"}}};
    BookOrder order;
    order.id = newId("ob_");
    order.pair = pair.symbol();
    order.side = lower(text(request, "side"));
    order.type = lower(text(request, "type").empty() ? "limit" : text(request, "type"));
    order.account = lower(text(request, "account"));
    order.baseWallet = lower(text(request, "baseWallet"));
    order.quoteWallet = lower(text(request, "quoteWallet"));
    order.baseDecimals = pair.base.decimals;
    order.quoteDecimals = pair.quote.decimals;

    if (order.side != "buy" && order.side != "sell") return {400, {{"error", "side : buy ou sell"}}};
    if (order.type != "limit" && order.type != "market") return {400, {{"error", "type : limit ou market"}}};
    if (!isAddress(order.account) || !isAddress(order.baseWallet) || !isAddress(order.quoteWallet))
        return {400, {{"error", "account, baseWallet et quoteWallet : adresses 0x requises"}}};
    if (order.baseWallet == order.quoteWallet)
        return {400, {{"error", "Un wallet par jeton : baseWallet et quoteWallet doivent être différents"}}};
    if (pair.quote.symbol == "VEZ" && order.quoteWallet != order.account)
        return {400, {{"error", "Le wallet de cotation VEZ doit être le sous-wallet VEZ du client"}}};

    auto amount = U512::parseUnits(text(request, "amount"), pair.base.decimals);
    if (!amount || amount->isZero()) return {400, {{"error", "Quantité invalide (" + std::to_string(pair.base.decimals) + " décimales maximum)"}}};
    order.amount = *amount;
    if (order.type == "limit") {
        auto price = U512::parseUnits(text(request, "price"), kPriceDecimals);
        if (!price || price->isZero()) return {400, {{"error", "Prix invalide (en " + pair.quote.symbol + " par " + pair.base.symbol + ")"}}};
        order.price = *price;
        if (quoteOf(order.amount, order.price, order.baseDecimals, order.quoteDecimals).isZero())
            return {400, {{"error", "Ordre trop petit : le total en " + pair.quote.symbol + " serait nul"}}};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (auto reason = suspensionLocked(order.account)) return {403, {{"error", *reason}}};

    std::map<std::string, std::optional<U512>> balances;
    U512 budget;   // achat : cotation disponible pour l'ordre
    if (order.side == "sell") {
        auto free = available(order.baseWallet, balances);
        if (!free) return {503, {{"error", "Solde du wallet " + pair.base.symbol + " illisible sur Slura"}}};
        if (*free < order.amount)
            return {422, {{"error", "Solde " + pair.base.symbol + " disponible insuffisant : " + free->formatUnits(pair.base.decimals)}}};
    } else {
        auto free = available(order.quoteWallet, balances);
        if (!free) return {503, {{"error", "Solde du wallet " + pair.quote.symbol + " illisible sur Slura"}}};
        budget = *free;
        if (order.type == "limit") {
            const U512 need = quoteOf(order.amount, order.price, order.baseDecimals, order.quoteDecimals, true);
            if (budget < need)
                return {422, {{"error", "Solde " + pair.quote.symbol + " disponible insuffisant : " + budget.formatUnits(pair.quote.decimals)}}};
        } else if (budget.isZero()) {
            return {422, {{"error", "Aucun " + pair.quote.symbol + " disponible"}}};
        }
    }

    // Contreparties : meilleur prix, puis ancienneté ; jamais ses propres ordres
    std::vector<BookOrder*> makers;
    for (auto& o : orders_)
        if (openStatus(o.status) && o.pair == order.pair && o.side != order.side && o.account != order.account) makers.push_back(&o);
    std::sort(makers.begin(), makers.end(), [&](const BookOrder* a, const BookOrder* b) {
        if (a->price != b->price) return order.side == "buy" ? a->price < b->price : a->price > b->price;
        return a->seq < b->seq;
    });

    const int64_t now = nowEpoch();
    U512 spent;
    json created = json::array();
    for (BookOrder* maker : makers) {
        if (order.remaining().isZero()) break;
        if (order.type == "limit" && (order.side == "buy" ? maker->price > order.price : maker->price < order.price)) break;

        // L'ordre en attente doit toujours être couvert sur la chaîne, sinon il est retiré
        const std::string makerWallet = maker->side == "sell" ? maker->baseWallet : maker->quoteWallet;
        auto it = balances.find(makerWallet);
        if (it == balances.end()) it = balances.emplace(makerWallet, config_.walletUnits(makerWallet)).first;
        if (!it->second) continue;   // RPC indisponible : on ne retire rien
        if (*it->second < committed(makerWallet)) {
            maker->status = "cancelled";
            maker->reason = "insufficient_balance";
            maker->updatedAt = nowIso();
            continue;
        }

        U512 qty = U512::min(order.remaining(), maker->remaining());
        U512 quote = quoteOf(qty, maker->price, order.baseDecimals, order.quoteDecimals);
        if (order.side == "buy" && order.type == "market" && quote > budget - spent) {
            qty = U512::min(qty, qtyFor(budget - spent, maker->price, order.baseDecimals, order.quoteDecimals));
            quote = quoteOf(qty, maker->price, order.baseDecimals, order.quoteDecimals);
        }
        if (qty.isZero() || quote.isZero()) break;

        BookTrade t;
        t.id = newId("tr_");
        t.pair = order.pair;
        t.status = "awaiting_delivery";
        t.takerSide = order.side;
        const BookOrder& buy = order.side == "buy" ? order : *maker;
        const BookOrder& sell = order.side == "sell" ? order : *maker;
        t.buyOrder = buy.id;
        t.sellOrder = sell.id;
        t.buyer = buy.account;
        t.buyerBaseWallet = buy.baseWallet;
        t.buyerQuoteWallet = buy.quoteWallet;
        t.seller = sell.account;
        t.sellerBaseWallet = sell.baseWallet;
        t.sellerQuoteWallet = sell.quoteWallet;
        t.baseDecimals = order.baseDecimals;
        t.quoteDecimals = order.quoteDecimals;
        t.price = maker->price;
        t.baseAmount = qty;
        t.quoteAmount = quote;
        t.fee = U512::mulDiv(quote, U512(config_.feeRatePpm), U512(1000000));
        t.feeRecipient = config_.feeRecipient;
        t.createdEpoch = now;
        t.createdAt = isoFromEpoch(now);
        t.deliveryDeadline = now + config_.deliverySeconds;

        maker->filled = maker->filled + qty;
        maker->updatedAt = t.createdAt;
        if (maker->remaining().isZero()) maker->status = "filled";
        order.filled = order.filled + qty;
        spent = spent + quote;
        trades_.push_back(t);
        created.push_back(tradeJson(t, order.account));
    }

    order.seq = ++seq_;
    order.createdAt = order.updatedAt = nowIso();
    if (order.remaining().isZero()) {
        order.status = "filled";
    } else if (order.type == "limit") {
        order.status = "open";   // reste dans le carnet
    } else {
        order.status = order.filled.isZero() ? "cancelled" : "filled";
        order.reason = order.filled.isZero() ? "no_liquidity" : "remainder_cancelled";
    }
    orders_.push_back(order);
    save();
    return {201, {{"order", orderJson(order)}, {"trades", created}}};
}

std::pair<int, json> OrderBook::cancel(const std::string& orderId, const std::string& account) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& o : orders_) {
        if (o.id != orderId) continue;
        if (o.account != lower(account)) return {403, {{"error", "Cet ordre n'appartient pas à ce compte"}}};
        if (!openStatus(o.status)) return {409, {{"error", "Ordre déjà " + o.status}}};
        o.status = "cancelled";
        o.reason = "cancelled_by_client";
        o.updatedAt = nowIso();
        save();
        return {200, orderJson(o)};
    }
    return {404, {{"error", "Ordre introuvable"}}};
}

void OrderBook::cancelPair(const std::string& pair, const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = false;
    for (auto& o : orders_) {
        if (o.pair != pair || !openStatus(o.status)) continue;
        o.status = "cancelled";
        o.reason = reason;
        o.updatedAt = nowIso();
        changed = true;
    }
    if (changed) save();
}

// ── Règlement ──

std::optional<BookTrade> OrderBook::trade(const std::string& tradeId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& t : trades_)
        if (t.id == tradeId) return t;
    return std::nullopt;
}

std::pair<int, json> OrderBook::deliver(const std::string& tradeId, const std::string& account, const std::string& opHash,
                                        const VerifiedTransfer& op) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& t : trades_) {
        if (t.id != tradeId) continue;
        if (t.seller != lower(account)) return {403, {{"error", "Seul le vendeur livre cet échange"}}};
        if (t.status != "awaiting_delivery") return {409, {{"error", "Livraison impossible : échange " + t.status}}};
        if (usedOps_.count(lower(opHash))) return {409, {{"error", "Cette opération a déjà servi"}}};
        if (op.state == VerifiedTransfer::State::Pending) return {202, {{"error", "Opération pas encore exécutée sur Slura"}, {"trade", tradeJson(t, account)}}};
        if (op.state != VerifiedTransfer::State::Ok) return {422, {{"error", op.error.empty() ? "Opération échouée" : op.error}}};
        if (op.sender != t.sellerBaseWallet) return {422, {{"error", "L'envoi ne part pas du wallet " + t.pair.substr(0, t.pair.find('/')) + " du vendeur"}}};
        if (op.to != t.buyerBaseWallet) return {422, {{"error", "L'envoi ne va pas au wallet de l'acheteur"}}};
        if (op.value < t.baseAmount) return {422, {{"error", "Quantité livrée insuffisante"}}};
        usedOps_.insert(lower(opHash));
        t.deliveryOp = lower(opHash);
        t.status = "awaiting_payment";
        t.deliveredAt = nowIso();
        t.paymentDeadline = nowEpoch() + config_.paymentSeconds;
        save();
        return {200, tradeJson(t, account)};
    }
    return {404, {{"error", "Échange introuvable"}}};
}

std::pair<int, json> OrderBook::pay(const std::string& tradeId, const std::string& account,
                                    const std::vector<std::pair<std::string, VerifiedTransfer>>& ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& t : trades_) {
        if (t.id != tradeId) continue;
        if (t.buyer != lower(account)) return {403, {{"error", "Seul l'acheteur paie cet échange"}}};
        if (t.status != "awaiting_payment" && t.status != "payment_overdue")
            return {409, {{"error", "Paiement impossible : échange " + t.status}}};
        U512 toSeller, toPool;
        for (const auto& [hash, op] : ops) {
            if (usedOps_.count(lower(hash))) return {409, {{"error", "L'opération " + hash + " a déjà servi"}}};
            if (op.state == VerifiedTransfer::State::Pending) return {202, {{"error", "Paiement pas encore exécuté sur Slura"}, {"trade", tradeJson(t, account)}}};
            if (op.state != VerifiedTransfer::State::Ok) return {422, {{"error", op.error.empty() ? "Opération échouée" : op.error}}};
            if (op.sender != t.buyerQuoteWallet) return {422, {{"error", "Le paiement ne part pas du wallet de l'acheteur"}}};
            if (op.to == t.sellerQuoteWallet) toSeller = toSeller + op.value;
            else if (op.to == t.feeRecipient) toPool = toPool + op.value;
            else return {422, {{"error", "Paiement vers un destinataire inattendu : " + op.to}}};
        }
        if (toSeller < t.sellerReceives()) return {422, {{"error", "Montant versé au vendeur insuffisant"}}};
        if (toPool < t.fee) return {422, {{"error", "Frais versés au pool insuffisants"}}};
        for (const auto& [hash, op] : ops) {
            usedOps_.insert(lower(hash));
            t.paymentOps.push_back(lower(hash));
        }
        t.status = "settled";
        t.settledAt = nowIso();
        save();
        return {200, tradeJson(t, account)};
    }
    return {404, {{"error", "Échange introuvable"}}};
}

void OrderBook::expire() {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t now = nowEpoch();
    bool changed = false;
    for (auto& t : trades_) {
        if (t.status == "awaiting_delivery" && now > t.deliveryDeadline) {
            t.status = "cancelled";   // rien n'a bougé : le vendeur n'a pas livré
            t.reason = "non_delivery";
            changed = true;
            // L'ordre d'achat à cours limité retrouve la quantité non livrée (le vendeur ne la retrouve pas)
            for (auto& o : orders_) {
                if (o.id != t.buyOrder || o.type != "limit" || o.status == "cancelled") continue;
                o.filled = o.filled > t.baseAmount ? o.filled - t.baseAmount : U512();
                o.status = "open";
                o.updatedAt = isoFromEpoch(now);
            }
        } else if (t.status == "awaiting_payment" && now > t.paymentDeadline) {
            t.status = "payment_overdue";   // l'acheteur a reçu la base : il reste redevable et suspendu
            changed = true;
        }
    }
    if (changed) save();
}

// ── Lecture ──

json OrderBook::orderJson(const BookOrder& o) const {
    json j = {
        {"id", o.id}, {"pair", o.pair}, {"side", o.side}, {"type", o.type}, {"status", o.status},
        {"amount", o.amount.formatUnits(o.baseDecimals)}, {"filled", o.filled.formatUnits(o.baseDecimals)},
        {"remaining", o.remaining().formatUnits(o.baseDecimals)}, {"account", o.account},
        {"baseWallet", o.baseWallet}, {"quoteWallet", o.quoteWallet}, {"created_at", o.createdAt},
        {"updated_at", o.updatedAt},
    };
    if (o.type == "limit") j["price"] = formatPrice(o.price);
    if (!o.reason.empty()) j["reason"] = o.reason;
    return j;
}

json OrderBook::tradeJson(const BookTrade& t, const std::string& account) const {
    json j = {
        {"id", t.id}, {"pair", t.pair}, {"status", t.status}, {"price", formatPrice(t.price)},
        {"amount", t.baseAmount.formatUnits(t.baseDecimals)}, {"total", t.quoteAmount.formatUnits(t.quoteDecimals)},
        {"fee", t.fee.formatUnits(t.quoteDecimals)}, {"sellerReceives", t.sellerReceives().formatUnits(t.quoteDecimals)},
        {"takerSide", t.takerSide}, {"created_at", t.createdAt},
    };
    const std::string who = lower(account);
    if (!who.empty() && (who == t.buyer || who == t.seller)) {
        const bool seller = who == t.seller;
        j["role"] = seller ? "seller" : "buyer";
        // Ce que l'app de ce client doit envoyer avec le SDK (unités exactes pour disburse)
        if (seller && t.status == "awaiting_delivery") {
            j["action"] = {{"step", "deliver"}, {"from", t.sellerBaseWallet}, {"transfers", json::array({
                {{"to", t.buyerBaseWallet}, {"units", t.baseAmount.toDecimalString()}, {"amount", t.baseAmount.formatUnits(t.baseDecimals)}}})},
                {"deadline", isoFromEpoch(t.deliveryDeadline)}};
        } else if (!seller && (t.status == "awaiting_payment" || t.status == "payment_overdue")) {
            json transfers = json::array({{{"to", t.sellerQuoteWallet}, {"units", t.sellerReceives().toDecimalString()},
                                           {"amount", t.sellerReceives().formatUnits(t.quoteDecimals)}}});
            if (!t.fee.isZero())
                transfers.push_back({{"to", t.feeRecipient}, {"units", t.fee.toDecimalString()}, {"amount", t.fee.formatUnits(t.quoteDecimals)}});
            j["action"] = {{"step", "pay"}, {"from", t.buyerQuoteWallet}, {"transfers", transfers},
                           {"deadline", isoFromEpoch(t.paymentDeadline)}};
        } else if (!seller && t.status == "awaiting_delivery") {
            j["waiting"] = "delivery";
            j["deadline"] = isoFromEpoch(t.deliveryDeadline);
        } else if (seller && (t.status == "awaiting_payment" || t.status == "payment_overdue")) {
            j["waiting"] = "payment";
        }
    }
    if (!t.reason.empty()) j["reason"] = t.reason;
    if (!t.deliveryOp.empty()) j["deliveryOp"] = t.deliveryOp;
    if (!t.paymentOps.empty()) j["paymentOps"] = t.paymentOps;
    if (!t.settledAt.empty()) j["settled_at"] = t.settledAt;
    return j;
}

json OrderBook::orders(const std::string& account) {
    std::lock_guard<std::mutex> lock(mutex_);
    json out = json::array();
    const std::string who = lower(account);
    for (auto it = orders_.rbegin(); it != orders_.rend(); ++it)
        if (it->account == who) out.push_back(orderJson(*it));
    return out;
}

json OrderBook::trades(const std::string& account) {
    std::lock_guard<std::mutex> lock(mutex_);
    json out = json::array();
    const std::string who = lower(account);
    for (auto it = trades_.rbegin(); it != trades_.rend(); ++it)
        if (it->buyer == who || it->seller == who) out.push_back(tradeJson(*it, who));
    return out;
}

json OrderBook::depth(const Pair& pair, size_t levels) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<U512, std::pair<U512, int>> bids, asks;
    for (const auto& o : orders_) {
        if (o.pair != pair.symbol() || !openStatus(o.status)) continue;
        auto& side = o.side == "buy" ? bids : asks;
        auto& level = side[o.price];
        level.first = level.first + o.remaining();
        level.second += 1;
    }
    auto render = [&](auto begin, auto end) {
        json out = json::array();
        for (auto it = begin; it != end && out.size() < levels; ++it)
            out.push_back({{"price", formatPrice(it->first)}, {"amount", it->second.first.formatUnits(pair.base.decimals)},
                           {"orders", it->second.second}});
        return out;
    };
    return {{"pair", pair.symbol()}, {"bids", render(bids.rbegin(), bids.rend())}, {"asks", render(asks.begin(), asks.end())}};
}

json OrderBook::recentTrades(const Pair& pair, size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    json out = json::array();
    for (auto it = trades_.rbegin(); it != trades_.rend() && out.size() < limit; ++it) {
        if (it->pair != pair.symbol() || it->status == "cancelled") continue;
        out.push_back({{"id", it->id}, {"price", formatPrice(it->price)}, {"amount", it->baseAmount.formatUnits(it->baseDecimals)},
                       {"side", it->takerSide}, {"status", it->status}, {"time", it->createdAt}});
    }
    return out;
}

json OrderBook::ticker(const Pair& pair, std::optional<double> indexVez) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t since = nowEpoch() - 86400;
    std::optional<U512> last, open24, high, low, bid, ask;
    long double volumeBase = 0, volumeQuote = 0, buyValue = 0, sellValue = 0;
    for (const auto& t : trades_) {
        if (t.pair != pair.symbol() || t.status == "cancelled") continue;
        last = t.price;
        if (t.createdEpoch < since) continue;
        if (!open24) open24 = t.price;
        if (!high || t.price > *high) high = t.price;
        if (!low || t.price < *low) low = t.price;
        volumeBase += t.baseAmount.toLongDouble(t.baseDecimals);
        const long double quote = t.quoteAmount.toLongDouble(t.quoteDecimals);
        volumeQuote += quote;
        (t.takerSide == "buy" ? buyValue : sellValue) += quote;
    }
    for (const auto& o : orders_) {
        if (o.pair != pair.symbol() || !openStatus(o.status)) continue;
        const long double value = quoteOf(o.remaining(), o.price, o.baseDecimals, o.quoteDecimals).toLongDouble(o.quoteDecimals);
        if (o.side == "buy") {
            buyValue += value;
            if (!bid || o.price > *bid) bid = o.price;
        } else {
            sellValue += value;
            if (!ask || o.price < *ask) ask = o.price;
        }
    }
    auto fmt = [](const std::optional<U512>& v) { return v ? json(formatPrice(*v)) : json(nullptr); };
    json j = {{"pair", pair.symbol()}, {"last", fmt(last)}, {"bid", fmt(bid)}, {"ask", fmt(ask)}, {"high24h", fmt(high)},
              {"low24h", fmt(low)}, {"volume24h", static_cast<double>(volumeBase)}, {"quoteVolume24h", static_cast<double>(volumeQuote)}};
    if (last && open24) {
        const long double from = open24->toLongDouble(kPriceDecimals), to = last->toLongDouble(kPriceDecimals);
        j["change24h"] = from > 0 ? static_cast<double>((to / from - 1) * 100) : 0.0;
    } else {
        j["change24h"] = nullptr;
    }
    // Cours de référence externe et prix de marque (référence, sinon dernier échange)
    j["index"] = indexVez ? json(*indexVez) : json(nullptr);
    if (indexVez) j["mark"] = *indexVez;
    else if (last) j["mark"] = static_cast<double>(last->toLongDouble(kPriceDecimals));
    else j["mark"] = nullptr;
    // Sentiment : part acheteuse (ordres d'achat ouverts + achats des dernières 24 h, en valeur)
    const long double total = buyValue + sellValue;
    const double buyPct = total > 0 ? static_cast<double>(buyValue / total * 100) : 50.0;
    j["sentiment"] = {{"buy", std::round(buyPct * 10) / 10}, {"sell", std::round((100 - buyPct) * 10) / 10}};
    return j;
}

json OrderBook::candles(const Pair& pair, int64_t intervalSeconds, size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    struct Candle {
        U512 open, high, low, close;
        long double volume = 0;
    };
    std::map<int64_t, Candle> buckets;
    for (const auto& t : trades_) {
        if (t.pair != pair.symbol() || t.status == "cancelled") continue;
        const int64_t start = t.createdEpoch / intervalSeconds * intervalSeconds;
        auto [it, inserted] = buckets.try_emplace(start);
        Candle& c = it->second;
        if (inserted) c.open = c.high = c.low = t.price;
        if (t.price > c.high) c.high = t.price;
        if (t.price < c.low) c.low = t.price;
        c.close = t.price;
        c.volume += t.baseAmount.toLongDouble(t.baseDecimals);
    }
    json out = json::array();
    size_t skip = buckets.size() > limit ? buckets.size() - limit : 0;
    for (const auto& [start, c] : buckets) {
        if (skip) {
            --skip;
            continue;
        }
        out.push_back({{"time", isoFromEpoch(start)}, {"open", formatPrice(c.open)}, {"high", formatPrice(c.high)},
                       {"low", formatPrice(c.low)}, {"close", formatPrice(c.close)}, {"volume", static_cast<double>(c.volume)}});
    }
    return out;
}

}  // namespace vyfttrade

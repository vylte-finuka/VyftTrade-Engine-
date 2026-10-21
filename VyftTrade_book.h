// VyftTrade_book.h : carnets d'ordres Vyft Trade (non dépositaire, règlement par le SDK dans l'app)
//
// Vyft Trade ne détient ni ne bloque aucun fonds. Un ordre est accepté si le wallet du client couvre,
// sur la chaîne, l'ordre en plus de ce que ses ordres et échanges en cours engagent déjà.
// Appariement : meilleur prix, puis ordre d'arrivée ; l'échange se fait au prix de l'ordre en attente.
//
// Règlement d'un échange (vendeur d'abord), chaque étape vérifiée sur l'opération SDK déclarée :
//   1. awaiting_delivery : l'app du vendeur envoie la base (disburse) de son wallet d'actif vers le
//      wallet d'actif de l'acheteur — délai deliverySeconds, sinon l'échange est annulé (rien n'a bougé) ;
//   2. awaiting_payment : l'app de l'acheteur envoie, depuis son wallet de cotation, le prix moins les
//      frais au vendeur et les frais au pool — délai paymentSeconds, sinon payment_overdue : l'acheteur
//      est suspendu jusqu'à son paiement ;
//   3. settled.
// Les frais (en cotation, VEZ par défaut) sont à la charge du vendeur, qui reçoit les VEZ.

#pragma once

#include "VyftTrade_amount.h"
#include "VyftTrade_market.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace vyfttrade {

// Prix : unités de cotation pour 1 jeton de base entier, × 10^18
inline constexpr int kPriceDecimals = 18;

struct BookOrder {
    std::string id;
    std::string pair;
    std::string side;      // buy | sell
    std::string type;      // limit | market
    std::string status;    // open | filled | cancelled
    std::string account;   // sous-wallet VEZ du client (identité)
    std::string baseWallet;
    std::string quoteWallet;
    int baseDecimals = 18;
    int quoteDecimals = 18;
    U512 price;            // 0 pour un ordre au marché
    U512 amount;           // en unités de la base
    U512 filled;
    std::string reason;
    std::string createdAt;
    std::string updatedAt;
    int64_t seq = 0;

    U512 remaining() const { return amount - filled; }
};

struct BookTrade {
    std::string id;
    std::string pair;
    std::string status;    // awaiting_delivery | awaiting_payment | payment_overdue | settled | cancelled
    std::string takerSide;
    std::string buyOrder, sellOrder;
    std::string buyer, buyerBaseWallet, buyerQuoteWallet;
    std::string seller, sellerBaseWallet, sellerQuoteWallet;
    int baseDecimals = 18;
    int quoteDecimals = 18;
    U512 price;
    U512 baseAmount;
    U512 quoteAmount;
    U512 fee;              // retenu sur le vendeur, versé au pool
    std::string feeRecipient;
    std::string deliveryOp;
    std::vector<std::string> paymentOps;
    std::string reason;
    std::string createdAt;
    int64_t createdEpoch = 0;
    int64_t deliveryDeadline = 0;
    int64_t paymentDeadline = 0;
    std::string deliveredAt;
    std::string settledAt;

    U512 sellerReceives() const { return quoteAmount - fee; }
};

// Opération SDK (UserOperation) déclarée par l'app, lue sur Slura
struct VerifiedTransfer {
    enum class State { Ok, Pending, Failed, Invalid } state = State::Invalid;
    std::string sender;    // wallet qui envoie
    std::string to;        // destinataire du disburse
    U512 value;            // montant en unités du jeton
    std::string error;
};

struct BookConfig {
    std::function<std::optional<U512>(const std::string& wallet)> walletUnits;   // solde sur la chaîne
    std::string feeRecipient;          // pool VyftHealth
    uint64_t feeRatePpm = 2000;        // frais du vendeur : 0,2 %
    int64_t deliverySeconds = 600;
    int64_t paymentSeconds = 900;
};

class OrderBook {
public:
    OrderBook(std::filesystem::path file, BookConfig config);

    // Ordre : { pair, side, type, amount, price?, account, baseWallet, quoteWallet }
    std::pair<int, nlohmann::json> place(const Pair& pair, const nlohmann::json& request);
    std::pair<int, nlohmann::json> cancel(const std::string& orderId, const std::string& account);
    void cancelPair(const std::string& pair, const std::string& reason);

    // Étapes de règlement : opération(s) SDK déjà lues sur la chaîne par le serveur
    std::pair<int, nlohmann::json> deliver(const std::string& tradeId, const std::string& account,
                                           const std::string& opHash, const VerifiedTransfer& op);
    std::pair<int, nlohmann::json> pay(const std::string& tradeId, const std::string& account,
                                       const std::vector<std::pair<std::string, VerifiedTransfer>>& ops);
    std::optional<BookTrade> trade(const std::string& tradeId);

    // Délais dépassés (appelé par la surveillance)
    void expire();

    nlohmann::json orders(const std::string& account);
    nlohmann::json trades(const std::string& account);
    nlohmann::json depth(const Pair& pair, size_t levels);
    nlohmann::json recentTrades(const Pair& pair, size_t limit);
    nlohmann::json ticker(const Pair& pair, std::optional<double> indexVez);
    nlohmann::json candles(const Pair& pair, int64_t intervalSeconds, size_t limit);
    std::optional<std::string> suspension(const std::string& account);

private:
    U512 committed(const std::string& wallet) const;
    std::optional<U512> available(const std::string& wallet, std::map<std::string, std::optional<U512>>& balances) const;
    std::optional<std::string> suspensionLocked(const std::string& account) const;
    nlohmann::json orderJson(const BookOrder& o) const;
    nlohmann::json tradeJson(const BookTrade& t, const std::string& account = "") const;
    void load();
    void save();

    std::filesystem::path file_;
    BookConfig config_;
    std::vector<BookOrder> orders_;
    std::vector<BookTrade> trades_;
    std::set<std::string> usedOps_;
    int64_t seq_ = 0;
    mutable std::mutex mutex_;
};

}  // namespace vyfttrade

// VyftTrade_motor.h : moteur d'ordres Vyft Trade™ (contexte VEZ / Slura)
//
// Construit, valide et envoie les ordres au format de l'API Vyft Trade :
// POST https://vyft-trade.vyft-one.com/api/v1/trade
//
// {
//   "assetdata": { "assetname": "Vyft VEZ", "asset": "VEZ", "chain": "SLU" },
//   "action": {
//     "typetrade": "buy" | "sell",
//     "typeaction": "execute_order" | "cancel_order",
//     "quantity": "12.5",
//     "currency": "EUR",
//     "priceincurrency": "1",
//     "wallet": "0x…",          // sous-wallet VEZ (Slura) qui règle l'ordre
//     "orderid": "…"            // obligatoire pour cancel_order
//   }
// }

#pragma once

#include <iostream>
#include <optional>
#include <string>

namespace vyfttrade {

// Point d'entrée de l'API Vyft Trade
inline constexpr const char* kApiHost = "vyft-trade.vyft-one.com";
inline constexpr const char* kTradePath = "/api/v1/trade";
inline const std::string kTradeUrl = std::string("https://") + kApiHost + kTradePath;

// Contexte VEZ : seul actif négocié pour l'instant, réglé sur Slura
inline constexpr const char* kVezAsset = "VEZ";
inline constexpr const char* kVezName = "Vyft VEZ";
inline constexpr const char* kVezChain = "SLU";
inline constexpr int kVezDecimals = 6;          // plus petite unité : 0,000001 VEZ
inline constexpr double kVezPriceEur = 1.0;     // 1 VEZ = 1 € (prix de référence)

enum class TradeType { Buy, Sell };
enum class ActionType { ExecuteOrder, CancelOrder };

struct AssetData {
    std::string assetName = kVezName;
    std::string asset = kVezAsset;
    std::string chain = kVezChain;
};

struct TradeAction {
    TradeType tradeType = TradeType::Buy;
    ActionType actionType = ActionType::ExecuteOrder;
    double quantity = 0.0;
    std::string currency = "EUR";
    double priceInCurrency = kVezPriceEur;   // prix d'1 VEZ dans la devise
    std::string wallet;                       // adresse 0x du sous-wallet VEZ
    std::string orderId;                      // pour cancel_order
};

struct Order {
    AssetData assetData;
    TradeAction action;
};

struct TradeResult {
    bool ok = false;
    long httpStatus = 0;
    std::string body;    // réponse brute de l'API
    std::string error;   // message en cas d'échec (validation ou réseau)
};

// Conversions texte ↔ énumérations (« buy », « execute_order »…)
std::optional<TradeType> parseTradeType(const std::string& value);
std::optional<ActionType> parseActionType(const std::string& value);
const char* toString(TradeType value);
const char* toString(ActionType value);

// Ordre VEZ prêt à l'emploi (actif, chaîne et prix de référence pré-remplis)
Order makeVezOrder(TradeType tradeType, ActionType actionType, double quantity,
                   const std::string& wallet, const std::string& currency = "EUR",
                   double priceInCurrency = kVezPriceEur);

// Vérifie l'ordre ; renvoie un message d'erreur, ou std::nullopt s'il est valide
std::optional<std::string> validateOrder(const Order& order);

// Quantité arrondie à l'unité VEZ (6 décimales), sans zéros inutiles : 12.5, 0.000001
std::string formatQuantity(double quantity);

// Corps JSON de la requête
std::string buildOrderJson(const Order& order);

// Valide puis envoie l'ordre (POST JSON) ; apiKey facultative (en-tête X-API-KEY)
TradeResult sendOrder(const Order& order, const std::string& url = kTradeUrl,
                      const std::string& apiKey = "");

}  // namespace vyfttrade

extern "C" {
// Interface C (appel depuis d'autres langages) : 0 = succès, sinon code d'erreur
//  1 = paramètre invalide, 2 = erreur réseau, 3 = refus de l'API
int Tradelive(const char* assetName, const char* tradeType, const char* actionType, double quantity,
              const char* currency, double priceInCurrency, const char* wallet);
int TradeliveVez(const char* tradeType, const char* actionType, double quantity, const char* wallet);
}

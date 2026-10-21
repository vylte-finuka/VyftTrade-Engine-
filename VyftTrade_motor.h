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
//     "leverage": "4",          // effet de levier (1 par défaut)
//     "stoploss": "0.00098",    // prix de stop (EUR) ; absent = stop automatique
//     "orderid": "…"            // obligatoire pour cancel_order
//   }
// }
//
// Positions à effet de levier (mise sans livraison d'actif) :
//  - execute_order ouvre une position : buy = à la hausse, sell = à la baisse ; quantity = mise en VEZ.
//    La mise reste sur le wallet du client, aucun VEZ n'est transféré à l'ouverture.
//  - Stop loss : la position est fermée sans perte ni gain (résultat nul).
//  - Gain brut : mise × levier × variation du prix. Ni le gain ni le levier n'ont de plafond fixe :
//    seule l'offre du pool les limite. À l'ouverture, la position réserve dans le pool son gain
//    maximum (mise × levier × kGainCapMove) et se clôt automatiquement quand elle l'atteint.
//  - Frais dynamiques sur le profit : taux entre kProfitFeeMin et kProfitFeeMax selon la tension
//    du pool (liquidité de trading entamée, gains réservés) ; les frais restent dans le pool
//    VyftHealth, le client reçoit le gain net (versé par l'app avec le SDK vyfttxl_mate).
//  - Levier maximum : offre disponible ÷ (mise × kGainCapMove × (1 − kProfitFeeMin)), l'offre étant
//    la liquidité de trading libre, bornée par le solde du pool et le plafond de versement du contrat
//    (lus sur la chaîne).
//  - cancel_order clôt la position (orderid) au prix courant.

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
inline constexpr double kVezPriceEur = 1.0;     // prix de repli (le serveur lit la preuve de réserves)

// Variation de prix couverte par position : le gain est plafonné à mise × levier × 50 %
// (la position est clôturée automatiquement à ce plafond)
inline constexpr double kGainCapMove = 0.5;

// Frais sur le profit : 10 % quand le pool est intact, jusqu'à 90 % quand il est épuisé ou engagé
inline constexpr double kProfitFeeMin = 0.10;
inline constexpr double kProfitFeeMax = 0.90;

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
    double leverage = 1.0;                    // effet de levier (≥ 1)
    double stopLoss = 0.0;                    // prix de stop en EUR (0 = automatique)
};

// Position ouverte : mise (VEZ), sens, levier, prix d'entrée et de stop (EUR)
struct Position {
    TradeType side = TradeType::Buy;
    double stake = 0.0;
    double leverage = 1.0;
    double entryPrice = 0.0;
    double stopLoss = 0.0;
};

enum class CloseReason { None, StopLoss, TakeProfit };

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

// Requêtes HTTP(S) brutes (WinHTTP sous Windows, libcurl ailleurs) : preuve de réserves, RPC Slura
TradeResult httpGet(const std::string& url);
TradeResult httpPostJson(const std::string& url, const std::string& body, const std::string& apiKey = "");

// ── Positions à effet de levier ──

// Levier maximum pour une mise, selon l'offre disponible (0 = position impossible)
//   maxTransferVez : plafond d'un versement du contrat (0 = aucun)
double maxLeverage(double stake, double availableVez, double maxTransferVez = 0);

// Gain brut maximum d'une position (VEZ)
double maxGain(double stake, double leverage);

// Versement net maximum, réservé dans le pool tant que la position est ouverte (frais minimum)
double maxPayout(double stake, double leverage);

// Taux de frais sur le profit selon la tension du pool :
//   health      = liquidité de trading restante ÷ liquidité de départ (1 = intacte, 0 = épuisée)
//   utilization = gains réservés ÷ liquidité de trading restante
// taux = kProfitFeeMin + (kProfitFeeMax − kProfitFeeMin) × max(1 − health, utilization)
double profitFeeRate(double health, double utilization);

// Partage d'un gain brut : frais (restent dans le pool) et net (versé au client), en unités VEZ
struct ProfitSplit {
    double gross = 0;
    double feeRate = 0;
    double fee = 0;
    double net = 0;
};
ProfitSplit splitProfit(double gross, double feeRate);

// Stop automatique : là où une position classique aurait perdu toute sa mise
double autoStopLoss(TradeType side, double entryPrice, double leverage);

// Vérifie une position avant ouverture ; renvoie un message d'erreur ou std::nullopt
std::optional<std::string> validatePosition(const Position& position, double availableVez,
                                            double maxTransferVez = 0);

// Résultat brut au prix donné (VEZ, négatif en cas de baisse pour un long)
double positionResult(const Position& position, double price);

// Clôture automatique au prix donné : stop loss atteint, ou plafond de gain atteint
CloseReason checkClose(const Position& position, double price);

// Gain brut à la clôture : 0 au stop loss ou en perte, gain plafonné sinon (avant frais)
double settlementAmount(const Position& position, double price, CloseReason reason);

const char* toString(CloseReason value);

}  // namespace vyfttrade

extern "C" {
// Interface C (appel depuis d'autres langages) : 0 = succès, sinon code d'erreur
//  1 = paramètre invalide, 2 = erreur réseau, 3 = refus de l'API
int Tradelive(const char* assetName, const char* tradeType, const char* actionType, double quantity,
              const char* currency, double priceInCurrency, const char* wallet);
int TradeliveVez(const char* tradeType, const char* actionType, double quantity, const char* wallet);
}

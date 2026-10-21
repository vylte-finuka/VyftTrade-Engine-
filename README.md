# Vyft Trade Engine™

Vyft Trade Engine™ est un moteur de trading robuste et flexible conçu pour faciliter les transactions d'actifs financiers via des requêtes API pour Vyft Trade. Il permet aux utilisateurs d'exécuter et d'annuler des ordres d'achat et de vente de manière efficace et sécurisée.

## Fonctionnalités

- **Exécution d'ordres d'achat et de vente** : Permet d'acheter ou de vendre des actifs financiers.
- **Annulation d'ordres** : Permet d'annuler des ordres existants.
- **Gestion des actifs financiers** : Gère les transactions pour divers actifs financiers.

## Contexte VEZ

Le moteur est adapté au VEZ, l'actif de Vyft réglé sur la chaîne Slura :

- **API** : `POST https://vyft-trade.vyft-one.com/api/v1/trade`
- **Actif** : `VEZ` uniquement (`assetname` « Vyft VEZ », `chain` « SLU ») ; tout autre actif est refusé.
- **Quantité** : décimale, au plus 6 décimales (plus petite unité 0,000001 VEZ).
- **Prix de référence** : 1 VEZ = 1 €, converti dans la devise du client (`currency`, `priceincurrency`).
- **Règlement** : `wallet` = adresse `0x…` du sous-wallet VEZ du client sur Slura.
- **Annulation** : `orderid` obligatoire avec `cancel_order`.

```json
{
  "assetdata": { "assetname": "Vyft VEZ", "asset": "VEZ", "chain": "SLU" },
  "action": {
    "typetrade": "buy",
    "typeaction": "execute_order",
    "quantity": "12.5",
    "currency": "EUR",
    "priceincurrency": "1",
    "wallet": "0x0000000000000000000000000000000000000000"
  }
}
```

### Compilation et exécution

```sh
cmake --preset x64-release && cmake --build out/build/x64-release
VyftTrade --dry-run buy execute_order 12.5 0x… EUR 1   # affiche la requête sans l'envoyer
VyftTrade sell execute_order 3 0x…                     # envoie l'ordre (clé facultative : VYFTTRADE_API_KEY)
```

Sous Windows l'envoi passe par WinHTTP ; ailleurs par libcurl. L'interface C `Tradelive(...)` /
`TradeliveVez(...)` renvoie 0 en cas de succès, 1 si l'ordre est invalide, 2 en cas d'erreur réseau,
3 si l'API refuse l'ordre.

### Serveur API

`VyftTradeServer [port=1542] [fichier=orders.json]` sert l'API derrière le proxy HTTPS de
`vyft-trade.vyft-one.com`.

Déploiement (Linux avec Docker) :

```sh
git clone https://github.com/vylte-finuka/VyftTrade-Engine-.git vyfttrade
cd vyfttrade
cp .env.example .env        # renseigner VYFTTRADE_POOL_ADDRESS et VYFTTRADE_POOL_SEED
docker compose up -d --build
curl http://127.0.0.1:1542/health
```

Le service écoute sur `127.0.0.1:1542` ; un proxy HTTPS (Caddy, nginx) le publie sur
`vyft-trade.vyft-one.com`. Les positions sont gardées dans le volume Docker `trade-data`.
Mise à jour : `git pull` puis `docker compose up -d --build`.

| Route | Rôle |
|---|---|
| `POST /api/v1/trade` | `execute_order` → ouvre une position (201) ; `cancel_order` → la clôt au prix courant (200) |
| `POST /api/v1/trade/payout` | `{orderid, wallet, txhash}` : déclare le versement du gain, vérifié sur Slura |
| `GET /api/v1/trade/quote?wallet=0x…&stake=N` | prix, offre du pool, frais actuels, levier maximum proposé |
| `GET /api/v1/trade/orders?wallet=0x…` | positions d'un sous-wallet VEZ |
| `GET /api/v1/trade/orders/<orderid>` | une position |
| `GET /health` | état du service |

**Positions.** La mise (`quantity`, en VEZ) reste sur le wallet du client. `typetrade` : `buy` à la
hausse, `sell` à la baisse ; `leverage` et `stoploss` facultatifs (stop automatique sinon).
- Stop loss : position fermée, **rien n'est perdu ni gagné** (`result: "nul"`).
- Gain : mise × levier × variation du prix. Ni le gain ni le levier n'ont de plafond fixe : l'offre
  du pool les limite. Chaque position réserve son gain maximum et se clôt d'elle-même à ce niveau.
- Frais dynamiques sur le profit (10 % à 90 % selon la tension du pool), qui restent dans le pool.

**Versement des gains.** Le serveur ne détient aucune clé. À la clôture d'une position gagnante, il
passe au statut `payout_due` avec le montant net (`payout`). L'app le verse depuis le pool VyftHealth
avec le SDK `vyfttxl_mate` (`redeemvez`), puis déclare la transaction sur `/api/v1/trade/payout`.
Le serveur vérifie l'événement `vezRedeemed` (pool, destinataire, montant) avant de passer la position
à `closed`. Un hash de transaction ne peut servir qu'à une seule position.

### Marché (paires et carnet d'ordres)

**Listing.** `POST /api/v1/admin/pairs` (en-tête `X-ADMIN-KEY`) liste une paire BASE/COTATION, cotée en
VEZ par défaut. Le jeton est vérifié sur Slura et ses décimales y sont lues ; `index` (facultatif) donne
la source du cours de référence. `DELETE /api/v1/admin/pairs/XAU-VEZ` la déliste et annule ses ordres.

```json
{ "base": { "symbol": "XAU", "name": "Or (once troy)", "token": "0x…" },
  "index": { "url": "https://cdn.jsdelivr.net/npm/@fawazahmed0/currency-api@latest/v1/currencies/xau.json",
             "path": "xau.eur", "currency": "EUR" } }
```

**Un wallet par jeton.** Chaque client a un wallet VyftVEZ par actif (le bytecode du wallet du SDK avec
l'adresse du jeton remplacée), créé par l'app. Vyft Trade ne détient ni ne bloque aucun fonds : un
ordre est accepté si le wallet couvre, sur la chaîne, l'ordre en plus de ses ordres et échanges en cours.

**Règlement, vendeur d'abord**, chaque étape étant une opération du SDK (`disburse`) vérifiée sur Slura :
1. `awaiting_delivery` : l'app du vendeur envoie la base vers le wallet d'actif de l'acheteur, puis
   `POST /api/v1/market/trades/<id>/delivery` `{account, op}`. Sans livraison dans le délai
   (`VYFTTRADE_DELIVERY_SECONDS`, 600), l'échange est annulé et l'ordre d'achat retrouve sa quantité.
2. `awaiting_payment` : l'app de l'acheteur envoie le prix moins les frais au vendeur et les frais au
   pool, puis `POST …/payment` `{account, ops}`. Hors délai (`VYFTTRADE_PAYMENT_SECONDS`, 900), l'échange
   passe à `payment_overdue` et l'acheteur est suspendu jusqu'à son paiement.
3. `settled`.

`GET /api/v1/market/trades?account=0x…` indique pour chaque échange l'étape que l'app du client doit
faire, avec les destinataires et les montants exacts. Les frais (`VYFTTRADE_TRADE_FEE_PPM`, 2000 =
0,2 %) sont à la charge du vendeur et vont au pool. Trois livraisons manquées en 24 h suspendent le vendeur.

**Données de marché.** `GET /api/v1/market/tickers` et, par paire, `…/ticker`, `…/book`, `…/trades`,
`…/candles?interval=1m|5m|15m|1h|4h|1d`. Le ticker donne le dernier prix, la variation, le volume sur
24 h, le cours de référence, le prix de marque et le sentiment (part acheteuse et vendeuse).

**Offre du pool.** Le serveur lit sur Slura le solde du pool, son plafond de versement
(`MAX_TRANSFER_AMOUNT`), `earnRatio` et les décimales du VEZ. La part réservée au trading est fixée par
`VYFTTRADE_POOL_SEED` (0 par défaut : trading fermé) ; les récompenses de pas n'y sont jamais prises.

| Variable | Défaut |
|---|---|
| `VYFTTRADE_POOL_SEED` | `0` |
| `VYFTTRADE_POOL_ADDRESS` | `0x352aBa76dDC0c4002134333417543fbb268b21a1` |
| `VYFTTRADE_ADMIN_KEY` | aucune (administration fermée) |
| `VYFT_RPC_URL` | `https://slu-charene.vyft-one.com` |
| `POR_URL` | `https://por.vyft-one.com/api/mint` |
| `VYFTTRADE_API_KEY` | aucune |
| `VYFTTRADE_POLL_SECONDS` | `15` |

Erreurs : 400 requête illisible, 401 clé d'API invalide (si `VYFTTRADE_API_KEY` est définie),
422 ordre ou versement refusé, 403/404/409 pour une clôture ou un versement impossible,
503 prix, pool ou RPC indisponible.

## Requête API (format général)

### Format de la Requête

La requête doit être envoyée en utilisant la méthode POST avec le format JSON suivant :

```json
{
  "assetdata": {
    "assetname": "Ethereum",
    "asset": "ETH"
  },
  "action": {
    "typetrade": "buy",
    "typeaction": "execute_order",
    "quantity": "100",
    "currency": "MGA",
    "priceincurrency": "10000000"
  }
}
Description des Champs
assetdata

assetname: Représente le nom de l'instrument financier. Exemple: "Ethereum"
asset: Symbole de l'actif financier. Exemple: "ETH"
action

typetrade: Type de transaction. Valeurs possibles: buy | sell
typeaction: Type d'action. Valeurs possibles: execute_order | cancel_order
quantity: Quantité de l'actif à acheter ou vendre. Exemple: "100"
currency: Devise utilisée pour le prix. Exemple: "MGA"
priceincurrency: Prix de l'actif dans la devise spécifiée. Exemple: "10000000"
Exemples de Requêtes
Exécution d'un ordre d'achat
json
Copier le code
{
  "assetdata": {
    "assetname": "Ethereum",
    "asset": "ETH"
  },
  "action": {
    "typetrade": "buy",
    "typeaction": "execute_order",
    "quantity": "100",
    "currency": "MGA",
    "priceincurrency": "10000000"
  }
}
Annulation d'un ordre
json
Copier le code
{
  "assetdata": {
    "assetname": "Ethereum",
    "asset": "ETH"
  },
  "action": {
    "typetrade": "sell",
    "typeaction": "cancel_order",
    "quantity": "100",
    "currency": "MGA",
    "priceincurrency": "10000000"
  }
}
Licence
sql
Copier le code
Vylte-finuka License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
© Vyft Trade Engine™
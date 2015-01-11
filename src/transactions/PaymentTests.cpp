// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
#include <asio.hpp>
#include "main/Application.h"
#include "util/Timer.h"
#include "main/Config.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "crypto/Base58.h"
#include "lib/json/json.h"
#include "TxTests.h"
#include "database/Database.h"
#include "ledger/LedgerMaster.h"

using namespace stellar;
using namespace stellar::txtest;


typedef std::unique_ptr<Application> appPtr;

// *STR Payment 
// *Credit Payment
// STR -> Credit Payment
// Credit -> STR Payment
// Credit -> STR -> Credit Payment
// Credit -> Credit -> Credit -> Credit Payment
// path payment where there isn't enough in the path
// path payment with a transfer rate
TEST_CASE("payment", "[tx][payment]")
{
    LOG(INFO) << "************ Starting payment test";

    Config const& cfg = getTestConfig();
    /*
    Config cfg2(cfg);
    //cfg2.DATABASE = "postgresql://dbmaster:-island-@localhost/hayashi";
    */

    VirtualClock clock;
    Application app(clock, cfg);
    app.start();

    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");
    SecretKey b1 = getAccount("B");

    uint64_t txfee = app.getLedgerMaster().getTxFee();

    const uint64_t paymentAmount = (uint64_t)app.getLedgerMaster().getMinBalance(0);

    TransactionFramePtr txFrame;

    txFrame = createPaymentTx(root, a1, 1, paymentAmount);
    TxDelta delta;
    txFrame->apply(delta, app);

    rapidjson::Value jsonResult;
    LedgerDelta ledgerDelta;

    delta.commitDelta(jsonResult, ledgerDelta, app.getLedgerMaster());

    REQUIRE(txFrame->getResultCode() == txSUCCESS);
    AccountFrame a1Account, rootAccount;
    REQUIRE(app.getDatabase().loadAccount(root.getPublicKey(), rootAccount));
    REQUIRE(app.getDatabase().loadAccount(a1.getPublicKey(), a1Account));
    REQUIRE(a1Account.getBalance() == paymentAmount);
    REQUIRE(rootAccount.getBalance() == (100000000000000 - paymentAmount - txfee));

    const uint64_t morePayment = paymentAmount / 2;

    SECTION("send STR to an existing account")
    {
        TransactionFramePtr txFrame2 = createPaymentTx(root, a1, 2, morePayment);
        TxDelta delta2;
        txFrame2->apply(delta2, app);

        rapidjson::Value jsonResult2;
        LedgerDelta ledgerDelta2;

        delta2.commitDelta(jsonResult2, ledgerDelta2, app.getLedgerMaster());

        REQUIRE(txFrame2->getResultCode() == txSUCCESS);
        AccountFrame a1Account2, rootAccount2;
        REQUIRE(app.getDatabase().loadAccount(root.getPublicKey(), rootAccount2));
        REQUIRE(app.getDatabase().loadAccount(a1.getPublicKey(), a1Account2));
        REQUIRE(a1Account2.getBalance() == a1Account.getBalance() + morePayment);
        REQUIRE(rootAccount2.getBalance() == (rootAccount.getBalance() - morePayment - txfee));
    }

    SECTION("send to self")
    {
        TransactionFramePtr txFrame2 = createPaymentTx(root, root, 2, morePayment);
        TxDelta delta2;
        txFrame2->apply(delta2, app);

        rapidjson::Value jsonResult2;
        LedgerDelta ledgerDelta2;

        delta2.commitDelta(jsonResult2, ledgerDelta2, app.getLedgerMaster());

        REQUIRE(txFrame2->getResultCode() == txSUCCESS);

        AccountFrame rootAccount2;
        REQUIRE(app.getDatabase().loadAccount(root.getPublicKey(), rootAccount2));
        REQUIRE(rootAccount2.getBalance() == (rootAccount.getBalance() - txfee));
    }

    SECTION("send too little STR to new account (below reserve)")
    {
        TransactionFramePtr txFrame2 = createPaymentTx(root, b1, 2,
            app.getLedgerMaster().getCurrentLedgerHeader().baseReserve -1);

        TxDelta delta2;
        txFrame2->apply(delta2, app);

        rapidjson::Value jsonResult2;
        LedgerDelta ledgerDelta2;

        delta2.commitDelta(jsonResult2, ledgerDelta2, app.getLedgerMaster());

        REQUIRE(txFrame2->getResultCode() == txUNDERFUNDED);
    }

    SECTION("simple credit")
    {
        Currency currency;
        currency.type(ISO4217);
        currency.isoCI().issuer = root.getPublicKey();
        strToCurrencyCode(currency.isoCI().currencyCode, "IDR");

        SECTION("credit sent to new account (no account error)")
        {
            txFrame = createCreditPaymentTx(root, b1, currency, 2, 100);
            TxDelta delta2;
            txFrame->apply(delta2, app);

            rapidjson::Value jsonResult2;
            LedgerDelta ledgerDelta2;

            delta2.commitDelta(jsonResult2, ledgerDelta2, app.getLedgerMaster());

            REQUIRE(txFrame->getResultCode() == txNOACCOUNT);
        }

        SECTION("send STR with path (malformed)")
        {
            TransactionFramePtr txFrame2 = createPaymentTx(root, a1, 2, morePayment);
            txFrame2->getEnvelope().tx.body.paymentTx().path.push_back(currency);
            TxDelta delta2;
            txFrame2->apply(delta2, app);

            rapidjson::Value jsonResult2;
            LedgerDelta ledgerDelta2;

            delta2.commitDelta(jsonResult2, ledgerDelta2, app.getLedgerMaster());

            REQUIRE(txFrame2->getResultCode() == txMALFORMED);
        }

        // actual sendcredit
        SECTION("credit payment with no trust")
        {

            txFrame = createCreditPaymentTx(root, a1, currency, 2, 100);
            TxDelta delta2;
            txFrame->apply(delta2, app);

            rapidjson::Value jsonResult2;
            LedgerDelta ledgerDelta2;

            delta2.commitDelta(jsonResult2, ledgerDelta2, app.getLedgerMaster());

            REQUIRE(txFrame->getResultCode() == txNOTRUST);
        }

        SECTION("with trust")
        {
            txFrame = setTrust(a1, root, 1, "IDR");
            TxDelta delta;
            txFrame->apply(delta, app);

            rapidjson::Value jsonResult;
            LedgerDelta ledgerDelta;

            delta.commitDelta(jsonResult, ledgerDelta, app.getLedgerMaster());

            REQUIRE(txFrame->getResultCode() == txSUCCESS);

            SECTION("simple credit payment")
            {
                Currency currency;
                currency.type(ISO4217);
                currency.isoCI().issuer = root.getPublicKey();
                strToCurrencyCode(currency.isoCI().currencyCode, "IDR");

                txFrame = createCreditPaymentTx(root, a1, currency, 3, 100);
                TxDelta delta;
                txFrame->apply(delta, app);

                rapidjson::Value jsonResult;
                LedgerDelta ledgerDelta;

                delta.commitDelta(jsonResult, ledgerDelta, app.getLedgerMaster());

                REQUIRE(txFrame->getResultCode() == txSUCCESS);
            }

        }
    }

    LOG(INFO) << "************ Ending payment test";
}




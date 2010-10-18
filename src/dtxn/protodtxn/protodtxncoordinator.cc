// Copyright 2008,2009,2010 Massachusetts Institute of Technology.
// All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "protodtxn/protodtxncoordinator.h"

#include <tr1/functional>

#include "base/assert.h"
#include "dtxn/dtxnmanager.h"
#include "dtxn/distributedtransaction.h"

using std::string;

using dtxn::DistributedTransaction;
using google::protobuf::Closure;
using google::protobuf::RpcController;

namespace protodtxn {

class ProtoDtxnCoordinatorRequest {
public:
    ProtoDtxnCoordinatorRequest(ProtoDtxnCoordinator* coordinator,
            int id,
            int num_partitions) :
            coordinator_(coordinator),
            controller_(NULL),
            response_(NULL),
            done_(NULL),
            id_(id),
            transaction_(new DistributedTransaction(num_partitions)),
            callback_(std::tr1::bind(&ProtoDtxnCoordinatorRequest::executeDone, this)),
            finish_callback_(std::tr1::bind(&ProtoDtxnCoordinatorRequest::finishDone, this)) {
        assert(coordinator_ != NULL);
    }

    ~ProtoDtxnCoordinatorRequest() {
        delete transaction_;
    }

    void setResponse(RpcController* controller, CoordinatorResponse* response, Closure* done) {
        fprintf(stderr, "%s:%d => Set Response Txn Id #%d\n", __FILE__, __LINE__, id_);
        assert(controller_ == NULL);
        assert(response_ == NULL);
        assert(done_ == NULL);
        //~ assert(controller != NULL);
        assert(response != NULL);
        assert(done != NULL);
        controller_ = controller;
        response_ = response;
        done_ = done;
    }

    DistributedTransaction* transaction() { return transaction_; }

    const std::tr1::function<void()>& callback() { return callback_; }
    const std::tr1::function<void()>& finish_callback() { return finish_callback_; }

    void setFinish(Closure* done) {
        CHECK(done_ == NULL);
        CHECK(done != NULL);
        done_ = done;
    }
    
private:
    void executeDone() {
        fprintf(stderr, "%s:%d => executeDone %d\n", __FILE__, __LINE__, id_);
        // Pass back the answers to the client
        // TODO: Need to handle more complicated things?
        for (int i = 0; i < transaction_->received().size(); ++i) {
            CoordinatorResponse::PartitionResponse* partition = response_->add_response();
            partition->set_partition_id(transaction_->received()[i].first);
            partition->set_output(transaction_->received()[i].second);
        }
        response_->set_transaction_id(id_);
        response_->set_status((FragmentResponse::Status) transaction_->status());

        transaction_->readyNextRound();

        // NULL all the fields so we don't accidentally touch them after the callback
        Closure* done = done_;
        controller_ = NULL;
        response_ = NULL;
        done_ = NULL;
        done->Run();
    }

    void finishDone() {
        Closure* done = done_;
        done_ = NULL;
        ProtoDtxnCoordinatorRequest* state = coordinator_->internalDeleteTransaction(id_);
        ASSERT(state == this);
        done->Run();
    }

    ProtoDtxnCoordinator* coordinator_;

    RpcController* controller_;
    CoordinatorResponse* response_;
    Closure* done_;

    int id_;
    DistributedTransaction* transaction_;
    std::tr1::function<void()> callback_;
    std::tr1::function<void()> finish_callback_;
};

ProtoDtxnCoordinator::ProtoDtxnCoordinator(dtxn::DtxnManager* dtxn_manager, int num_partitions) :
        dtxn_manager_(dtxn_manager),
        num_partitions_(num_partitions) {
    CHECK(dtxn_manager_ != NULL);
    CHECK(num_partitions_ > 0);
}

ProtoDtxnCoordinator::~ProtoDtxnCoordinator() {
}

void ProtoDtxnCoordinator::Execute(RpcController* controller,
        const CoordinatorFragment* request,
        CoordinatorResponse* response,
        Closure* done) {
    fprintf(stderr, "%s:%d => Execute %d\n", __FILE__, __LINE__, request->transaction_id());
    CHECK(request->fragment_size() > 0);
    assert(!response->IsInitialized());

    // TODO: Check that transaction ids are increasing or otherwise unique?
    ProtoDtxnCoordinatorRequest* state = NULL;
    TransactionMap::iterator it = transactions_.find(request->transaction_id());
    if (it == transactions_.end()) {
        state = new ProtoDtxnCoordinatorRequest(this, request->transaction_id(), num_partitions_);

        std::pair<TransactionMap::iterator, bool> result = transactions_.insert(
                std::make_pair(request->transaction_id(), state));
        CHECK(result.second);
    } else {
        state = it->second;
        CHECK(state->transaction()->multiple_partitions());
    }
    state->setResponse(controller, response, done);
    
    // PAVLO:
    if (state->transaction()->has_payload() == false && request->has_payload() == true) {
        fprintf(stderr, "%s:%d => Setting DistributedTransaction payload [%s]\n", __FILE__, __LINE__, request->payload().c_str());
        state->transaction()->set_payload(request->payload());
    }

    for (int i = 0; i < request->fragment_size(); ++i) {
        state->transaction()->send(
                request->fragment(i).partition_id(), request->fragment(i).work());
    }

    // TODO: Specify "done" at a finer granularity?
    if (request->last_fragment()) {
        state->transaction()->setAllDone();
    }

    dtxn_manager_->execute(state->transaction(), state->callback());
    state->transaction()->sentMessages();
}

void ProtoDtxnCoordinator::Finish(RpcController* controller,
        const FinishRequest* request,
        FinishResponse* response,
        Closure* done) {
    fprintf(stderr, "%s:%d => Finish %d [payload=%s]\n", __FILE__, __LINE__, request->transaction_id(), request->payload().c_str());
    
    // Finish this transaction
    TransactionMap::iterator it = transactions_.find(request->transaction_id());
    CHECK(it != transactions_.end());
    ProtoDtxnCoordinatorRequest* state = it->second;

    if (state->transaction()->multiple_partitions() &&
            state->transaction()->status() == DistributedTransaction::OK) {
        state->setFinish(done);
        fprintf(stderr, "%s:%d => Telling our DtxnManager that we want to finish\n", __FILE__, __LINE__);
        dtxn_manager_->finish(state->transaction(), request->commit(), request->payload(), state->finish_callback());
    } else {
        CHECK(request->commit() == (state->transaction()->status() == DistributedTransaction::OK));
        // This is a single partition transaction, or it aborted: just delete the state
        internalDeleteTransaction(request->transaction_id());
        done->Run();
    }
}

ProtoDtxnCoordinatorRequest* ProtoDtxnCoordinator::internalDeleteTransaction(int32_t transaction_id) {
    TransactionMap::iterator it = transactions_.find(transaction_id);
    CHECK(it != transactions_.end());

    ProtoDtxnCoordinatorRequest* state = it->second;
    transactions_.erase(it);
    delete state;
    return state;
}

}  // namespace protodtxn

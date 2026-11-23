// SPDX-License-Identifier: Apache-2.0
#include "server/publish_http.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using wsrv::ErrorCode;
using wsrv::PublishReport;

TEST(PublishHttp, ADeliveredPublishIsAnsweredWithTheCounts) {
    const auto answer =
        wsrv::publish_answer(PublishReport{.subscribers = 3, .accepted = 2, .refused = 1});
    EXPECT_EQ(answer.status, 200);  // partial refusal is still a 200: the counts say what happened
    EXPECT_EQ(answer.body, "subscribers=3 accepted=2 refused=1\n");
}

TEST(PublishHttp, NobodyListeningIsStillSuccess) {
    const auto answer = wsrv::publish_answer(PublishReport{});
    EXPECT_EQ(answer.status, 200);
    EXPECT_EQ(answer.body, "subscribers=0 accepted=0 refused=0\n");
}

TEST(PublishHttp, WhenEveryDeliveryWasRefusedTheCallerShouldRetry) {
    const auto answer =
        wsrv::publish_answer(PublishReport{.subscribers = 2, .accepted = 0, .refused = 2});
    EXPECT_EQ(answer.status, 429);
    EXPECT_EQ(answer.body, "subscribers=2 accepted=0 refused=2\n");
}

TEST(PublishHttp, BrokerErrorsMapToTheirStatus) {
    const auto status = [](ErrorCode code) {
        return wsrv::publish_answer(std::unexpected{wsrv::Error{code, "test"}}).status;
    };
    EXPECT_EQ(status(ErrorCode::InvalidUtf8), 415);
    EXPECT_EQ(status(ErrorCode::InvalidArgument), 400);
    EXPECT_EQ(status(ErrorCode::BufferFull), 429);
    EXPECT_EQ(status(ErrorCode::ResourceExhausted), 503);
    EXPECT_EQ(status(ErrorCode::Closing), 503);
}

TEST(PublishHttp, AResponseIsOneCompleteMessageThatCloses) {
    const std::string response = wsrv::publish_response(200, "hi\n");
    EXPECT_EQ(response,
              "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
              "Content-Length: 3\r\nConnection: close\r\n\r\nhi\n");
}

TEST(PublishHttp, A401NamesTheSchemeAndNothingElse) {
    const std::string response = wsrv::publish_response(401, "credential required\n");
    EXPECT_NE(response.find("WWW-Authenticate: Bearer\r\n"), std::string::npos);
    EXPECT_EQ(response.find("token"), std::string::npos);  // never a hint about the secret
}

TEST(PublishHttp, EveryAnsweredStatusHasACounterAndOthersDoNot) {
    for (const int status : {200, 400, 401, 404, 405, 411, 413, 415, 417, 429, 501, 503}) {
        EXPECT_TRUE(wsrv::publish_metric(status).has_value()) << status;
    }
    EXPECT_FALSE(wsrv::publish_metric(418).has_value());
    EXPECT_EQ(wsrv::metric_info(*wsrv::publish_metric(413)).label1_value, "413");
}

}  // namespace

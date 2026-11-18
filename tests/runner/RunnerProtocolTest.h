#pragma once

#include "protocol/FrameJson.h"
#include "protocol/Frames.h"

#include <QObject>

#include <functional>

namespace runner_protocol_test {

using namespace ketplus::runner::protocol;

// A valid frame of every type, and the direction it is sent in.
struct Sample {
    Frame frame;
    Direction direction;
};
[[nodiscard]] QList<Sample> samples();
[[nodiscard]] Direction senderOf(const Frame& frame);
[[nodiscard]] QString objectId(char digit = 'a');
[[nodiscard]] QString key(char digit = 'k');
[[nodiscard]] QString sha256(char digit = 'f');
[[nodiscard]] QByteArray compact(const QJsonObject& object);

} // namespace runner_protocol_test

Q_DECLARE_METATYPE(ketplus::runner::protocol::Frame)
Q_DECLARE_METATYPE(ketplus::runner::protocol::Direction)
Q_DECLARE_METATYPE(ketplus::runner::protocol::FrameError::Kind)

class RunnerProtocolTest : public QObject {
    Q_OBJECT

  private slots:
    // RunnerProtocolTest.cpp
    void everyFrameRoundTrips_data();
    void everyFrameRoundTrips();
    void optionalAndNullFieldsRoundTrip_data();
    void optionalAndNullFieldsRoundTrip();
    void routesMatchTheChannelTable();
    void decodeRefusesWrongChannelAndDirection();
    void bulkCarriesNoJsonFrame();
    void epochFollowsTheSender();
    void malformedInputIsRefused_data();
    void malformedInputIsRefused();
    void wrongFieldTypesAreRefused_data();
    void wrongFieldTypesAreRefused();
    void unknownFieldsAreIgnored();
    void unknownTypesAreUnsupported();
    void errorCodesConvertBothWays();
    void errorCodesBelongToTheirFrame();
    void helloWithoutTheProtocolCapability();
    void invalidSpecCarriesItsJobIdentity();
    void escapedLoneSurrogatesAreRefused();
    void duplicateWarningsAreDropped();
    void knownWarningsAreTheThreeCodes();

    // RunnerProtocolLimitsTest.cpp
    void frameSizeLimits_data();
    void frameSizeLimits();
    void anyMessageAboveTheLargestLimitIsOversize();
    void encodeRefusesAnOversizeFrame();
    void packLimitIsTheCallersCheck();
    void fieldLimits_data();
    void fieldLimits();
};

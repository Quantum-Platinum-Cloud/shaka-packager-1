// Copyright 2014 Google Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <gtest/gtest.h>
#include <inttypes.h>
#include <libxml/xmlstring.h>

#include "packager/base/file_util.h"
#include "packager/base/logging.h"
#include "packager/base/strings/string_number_conversions.h"
#include "packager/base/strings/string_util.h"
#include "packager/base/strings/stringprintf.h"
#include "packager/media/file/file.h"
#include "packager/mpd/base/mpd_builder.h"
#include "packager/mpd/base/mpd_utils.h"
#include "packager/mpd/test/mpd_builder_test_helper.h"
#include "packager/mpd/test/xml_compare.h"

namespace edash_packager {

using base::FilePath;

namespace {
const char kSElementTemplate[] =
    "<S t=\"%" PRIu64 "\" d=\"%" PRIu64 "\" r=\"%" PRIu64 "\"/>\n";
const char kSElementTemplateWithoutR[] =
    "<S t=\"%" PRIu64 "\" d=\"%" PRIu64 "\"/>\n";
const int kDefaultStartNumber = 1;

// Get 'id' attribute from |node|, convert it to std::string and convert it to a
// number.
void ExpectXmlElementIdEqual(xmlNodePtr node, uint32_t id) {
  const char kId[] = "id";
  xml::ScopedXmlPtr<xmlChar>::type id_attribute_xml_str(
      xmlGetProp(node, BAD_CAST kId));
  ASSERT_TRUE(id_attribute_xml_str);

  unsigned id_attribute_unsigned = 0;
  std::string id_attribute_str =
      reinterpret_cast<const char*>(id_attribute_xml_str.get());
  ASSERT_TRUE(base::StringToUint(id_attribute_str, &id_attribute_unsigned));

  ASSERT_EQ(id, id_attribute_unsigned);
}

// Using template to support both AdaptationSet and Representation.
template <typename T>
void CheckIdEqual(uint32_t expected_id, T* node) {
  ASSERT_EQ(expected_id, node->id());

  // Also check if the XML generated by libxml2 has the correct id attribute.
  xml::ScopedXmlPtr<xmlNode>::type node_xml(node->GetXml());
  ASSERT_NO_FATAL_FAILURE(ExpectXmlElementIdEqual(node_xml.get(), expected_id));
}
}  // namespace

template <MpdBuilder::MpdType type>
class MpdBuilderTest: public ::testing::Test {
 public:
  MpdBuilderTest() : mpd_(type, MpdOptions()), representation_() {}
  virtual ~MpdBuilderTest() {}

  void CheckMpd(const std::string& expected_output_file) {
    std::string mpd_doc;
    ASSERT_TRUE(mpd_.ToString(&mpd_doc));
    ASSERT_TRUE(ValidateMpdSchema(mpd_doc));

    ASSERT_NO_FATAL_FAILURE(
        ExpectMpdToEqualExpectedOutputFile(mpd_doc, expected_output_file));
  }

 protected:
  void AddRepresentation(const MediaInfo& media_info) {
    AdaptationSet* adaptation_set = mpd_.AddAdaptationSet("");
    ASSERT_TRUE(adaptation_set);

    Representation* representation =
        adaptation_set->AddRepresentation(media_info);
    ASSERT_TRUE(representation);

    representation_ = representation;
  }

  MpdBuilder mpd_;

  // We usually need only one representation.
  Representation* representation_;  // Owned by |mpd_|.

 private:
  DISALLOW_COPY_AND_ASSIGN(MpdBuilderTest);
};

class StaticMpdBuilderTest : public MpdBuilderTest<MpdBuilder::kStatic> {};

class DynamicMpdBuilderTest : public MpdBuilderTest<MpdBuilder::kDynamic> {
 public:
  virtual ~DynamicMpdBuilderTest() {}

  // Anchors availabilityStartTime so that the test result doesn't depend on the
  // current time.
  virtual void SetUp() {
    mpd_.availability_start_time_ = "2011-12-25T12:30:00";
  }

  MpdOptions* mutable_mpd_options() { return &mpd_.mpd_options_; }

  std::string GetDefaultMediaInfo() {
    const char kMediaInfo[] =
        "video_info {\n"
        "  codec: \"avc1.010101\"\n"
        "  width: 720\n"
        "  height: 480\n"
        "  time_scale: 10\n"
        "}\n"
        "reference_time_scale: %u\n"
        "container_type: 1\n"
        "init_segment_name: \"init.mp4\"\n"
        "segment_template: \"$Time$.mp4\"\n";

    return base::StringPrintf(kMediaInfo, DefaultTimeScale());
  }

  // TODO(rkuroiwa): Make this a global constant in anonymous namespace.
  uint32_t DefaultTimeScale() const { return 1000; };
};

class SegmentTemplateTest : public DynamicMpdBuilderTest {
 public:
  SegmentTemplateTest()
      : bandwidth_estimator_(BandwidthEstimator::kUseAllBlocks) {}
  virtual ~SegmentTemplateTest() {}

  virtual void SetUp() {
    DynamicMpdBuilderTest::SetUp();
    ASSERT_NO_FATAL_FAILURE(AddRepresentationWithDefaultMediaInfo());
  }

  void AddSegments(uint64_t start_time,
                   uint64_t duration,
                   uint64_t size,
                   uint64_t repeat) {
    DCHECK(representation_);

    SegmentInfo s = {start_time, duration, repeat};
    segment_infos_for_expected_out_.push_back(s);
    if (repeat == 0) {
      expected_s_elements_ +=
          base::StringPrintf(kSElementTemplateWithoutR, start_time, duration);
    } else {
      expected_s_elements_ +=
          base::StringPrintf(kSElementTemplate, start_time, duration, repeat);
    }

    for (uint64_t i = 0; i < repeat + 1; ++i) {
      representation_->AddNewSegment(start_time, duration, size);
      start_time += duration;
      bandwidth_estimator_.AddBlock(
          size, static_cast<double>(duration) / DefaultTimeScale());
    }
  }

 protected:
  void AddRepresentationWithDefaultMediaInfo() {
    ASSERT_NO_FATAL_FAILURE(
        AddRepresentation(ConvertToMediaInfo(GetDefaultMediaInfo())));
  }

  std::string TemplateOutputInsertValues(const std::string& s_elements_string,
                                         uint64_t bandwidth) {
    const char kOutputTemplate[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<MPD xmlns=\"urn:mpeg:DASH:schema:MPD:2011\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
        "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
        "xsi:schemaLocation=\"urn:mpeg:DASH:schema:MPD:2011 DASH-MPD.xsd\" "
        "availabilityStartTime=\"2011-12-25T12:30:00\" minBufferTime=\"PT2S\" "
        "type=\"dynamic\" profiles=\"urn:mpeg:dash:profile:isoff-live:2011\">\n"
        "  <Period start=\"PT0S\">\n"
        "    <AdaptationSet id=\"0\">\n"
        "      <Representation id=\"0\" bandwidth=\"%" PRIu64 "\" "
        "codecs=\"avc1.010101\" mimeType=\"video/mp4\" width=\"720\" "
        "height=\"480\">\n"
        "        <SegmentTemplate timescale=\"1000\" "
        "initialization=\"init.mp4\" media=\"$Time$.mp4\">\n"
        "          <SegmentTimeline>\n%s"
        "          </SegmentTimeline>\n"
        "        </SegmentTemplate>\n"
        "      </Representation>\n"
        "    </AdaptationSet>\n"
        "  </Period>\n"
        "</MPD>\n";

    return base::StringPrintf(kOutputTemplate,
                              bandwidth,
                              s_elements_string.c_str());
  }

  void CheckMpdAgainstExpectedResult() {
    std::string mpd_doc;
    ASSERT_TRUE(mpd_.ToString(&mpd_doc));
    ASSERT_TRUE(ValidateMpdSchema(mpd_doc));
    const std::string& expected_output =
        TemplateOutputInsertValues(expected_s_elements_,
                                   bandwidth_estimator_.Estimate());
    ASSERT_TRUE(XmlEqual(expected_output, mpd_doc))
        << "Expected " << expected_output << std::endl << "Actual: " << mpd_doc;
  }

  std::list<SegmentInfo> segment_infos_for_expected_out_;
  std::string expected_s_elements_;
  BandwidthEstimator bandwidth_estimator_;
};

class TimeShiftBufferDepthTest : public SegmentTemplateTest {
 public:
  TimeShiftBufferDepthTest() {}
  virtual ~TimeShiftBufferDepthTest() {}

  // This function is tricky. It does not call SegmentTemplateTest::Setup() so
  // that it does not automatically add a representation, that has $Time$
  // template.
  virtual void SetUp() {
    DynamicMpdBuilderTest::SetUp();

    // The only diff with current GetDefaultMediaInfo() is that this uses
    // $Number$ for segment template.
    const char kMediaInfo[] =
        "video_info {\n"
        "  codec: \"avc1.010101\"\n"
        "  width: 720\n"
        "  height: 480\n"
        "  time_scale: 10\n"
        "}\n"
        "reference_time_scale: %u\n"
        "container_type: 1\n"
        "init_segment_name: \"init.mp4\"\n"
        "segment_template: \"$Number$.mp4\"\n";

    const std::string& number_template_media_info =
        base::StringPrintf(kMediaInfo, DefaultTimeScale());
    ASSERT_NO_FATAL_FAILURE(
        AddRepresentation(ConvertToMediaInfo(number_template_media_info)));
  }

  void CheckTimeShiftBufferDepthResult(const std::string& expected_s_element,
                                       int expected_time_shift_buffer_depth,
                                       int expected_start_number) {
    const char kOutputTemplate[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<MPD xmlns=\"urn:mpeg:DASH:schema:MPD:2011\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
        "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
        "xsi:schemaLocation=\"urn:mpeg:DASH:schema:MPD:2011 DASH-MPD.xsd\" "
        "availabilityStartTime=\"2011-12-25T12:30:00\" minBufferTime=\"PT2S\" "
        "type=\"dynamic\" profiles=\"urn:mpeg:dash:profile:isoff-live:2011\" "
        "timeShiftBufferDepth=\"PT%dS\">\n"
        "  <Period start=\"PT0S\">\n"
        "    <AdaptationSet id=\"0\">\n"
        "      <Representation id=\"0\" bandwidth=\"%" PRIu64 "\" "
        "codecs=\"avc1.010101\" mimeType=\"video/mp4\" width=\"720\" "
        "height=\"480\">\n"
        "        <SegmentTemplate timescale=\"1000\" "
        "initialization=\"init.mp4\" media=\"$Number$.mp4\" "
        "startNumber=\"%d\">\n"
        "          <SegmentTimeline>\n"
        "              %s\n"
        "          </SegmentTimeline>\n"
        "        </SegmentTemplate>\n"
        "      </Representation>\n"
        "    </AdaptationSet>\n"
        "  </Period>\n"
        "</MPD>\n";

    std::string expected_out =
        base::StringPrintf(kOutputTemplate,
                           expected_time_shift_buffer_depth,
                           bandwidth_estimator_.Estimate(),
                           expected_start_number,
                           expected_s_element.c_str());

    std::string mpd_doc;
    ASSERT_TRUE(mpd_.ToString(&mpd_doc));
    ASSERT_TRUE(ValidateMpdSchema(mpd_doc));
    ASSERT_TRUE(XmlEqual(expected_out, mpd_doc))
        << "Expected " << expected_out << std::endl << "Actual: " << mpd_doc;
  }
};

TEST_F(StaticMpdBuilderTest, CheckAdaptationSetId) {
  base::AtomicSequenceNumber sequence_counter;
  const uint32_t kAdaptationSetId = 42;

  AdaptationSet adaptation_set(
      kAdaptationSetId, "", MpdOptions(), &sequence_counter);
  ASSERT_NO_FATAL_FAILURE(CheckIdEqual(kAdaptationSetId, &adaptation_set));
}

TEST_F(StaticMpdBuilderTest, CheckRepresentationId) {
  const MediaInfo video_media_info = GetTestMediaInfo(kFileNameVideoMediaInfo1);
  const uint32_t kRepresentationId = 1;

  Representation representation(
      video_media_info, MpdOptions(), kRepresentationId);
  EXPECT_TRUE(representation.Init());
  ASSERT_NO_FATAL_FAILURE(CheckIdEqual(kRepresentationId, &representation));
}

// Add one video check the output.
TEST_F(StaticMpdBuilderTest, Video) {
  MediaInfo video_media_info = GetTestMediaInfo(kFileNameVideoMediaInfo1);
  ASSERT_NO_FATAL_FAILURE(AddRepresentation(video_media_info));
  EXPECT_NO_FATAL_FAILURE(CheckMpd(kFileNameExpectedMpdOutputVideo1));
}

// Add both video and audio and check the output.
TEST_F(StaticMpdBuilderTest, VideoAndAudio) {
  MediaInfo video_media_info = GetTestMediaInfo(kFileNameVideoMediaInfo1);
  MediaInfo audio_media_info = GetTestMediaInfo(kFileNameAudioMediaInfo1);

  // The order matters here to check against expected output.
  AdaptationSet* video_adaptation_set = mpd_.AddAdaptationSet("");
  ASSERT_TRUE(video_adaptation_set);

  AdaptationSet* audio_adaptation_set = mpd_.AddAdaptationSet("");
  ASSERT_TRUE(audio_adaptation_set);

  Representation* audio_representation =
      audio_adaptation_set->AddRepresentation(audio_media_info);
  ASSERT_TRUE(audio_representation);

  Representation* video_representation =
      video_adaptation_set->AddRepresentation(video_media_info);
  ASSERT_TRUE(video_representation);

  EXPECT_NO_FATAL_FAILURE(CheckMpd(kFileNameExpectedMpdOutputAudio1AndVideo1));
}

// MPD schema has strict ordering. AudioChannelConfiguration must appear before
// ContentProtection.
TEST_F(StaticMpdBuilderTest, AudioChannelConfigurationWithContentProtection) {
  MediaInfo encrypted_audio_media_info =
      GetTestMediaInfo(kFileNameEncytpedAudioMediaInfo);

  AdaptationSet* audio_adaptation_set = mpd_.AddAdaptationSet("");
  ASSERT_TRUE(audio_adaptation_set);

  Representation* audio_representation =
      audio_adaptation_set->AddRepresentation(encrypted_audio_media_info);
  ASSERT_TRUE(audio_representation);

  EXPECT_NO_FATAL_FAILURE(CheckMpd(kFileNameExpectedMpdOutputEncryptedAudio));
}

// Static profile requires bandwidth to be set because it has no other way to
// get the bandwidth for the Representation.
TEST_F(StaticMpdBuilderTest, MediaInfoMissingBandwidth) {
  MediaInfo video_media_info = GetTestMediaInfo(kFileNameVideoMediaInfo1);
  video_media_info.clear_bandwidth();
  AddRepresentation(video_media_info);

  std::string mpd_doc;
  ASSERT_FALSE(mpd_.ToString(&mpd_doc));
}

TEST_F(StaticMpdBuilderTest, WriteToFile) {
  MediaInfo video_media_info = GetTestMediaInfo(kFileNameVideoMediaInfo1);
  AdaptationSet* video_adaptation_set = mpd_.AddAdaptationSet("");
  ASSERT_TRUE(video_adaptation_set);

  Representation* video_representation =
      video_adaptation_set->AddRepresentation(video_media_info);
  ASSERT_TRUE(video_representation);

  base::FilePath file_path;
  ASSERT_TRUE(base::CreateTemporaryFile(&file_path));
  media::File* file = media::File::Open(file_path.value().data(), "w");
  ASSERT_TRUE(file);
  ASSERT_TRUE(mpd_.WriteMpdToFile(file));
  ASSERT_TRUE(file->Close());

  std::string file_content;
  ASSERT_TRUE(base::ReadFileToString(file_path, &file_content));
  ASSERT_NO_FATAL_FAILURE(ExpectMpdToEqualExpectedOutputFile(
      file_content, kFileNameExpectedMpdOutputVideo1));

  const bool kNonRecursive = false;
  EXPECT_TRUE(DeleteFile(file_path, kNonRecursive));
}

// Check whether the attributes are set correctly for dynamic <MPD> element.
TEST_F(DynamicMpdBuilderTest, CheckMpdAttributes) {
  static const char kExpectedOutput[] =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<MPD xmlns=\"urn:mpeg:DASH:schema:MPD:2011\" "
      "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
      "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
      "xsi:schemaLocation=\"urn:mpeg:DASH:schema:MPD:2011 "
      "DASH-MPD.xsd\" minBufferTime=\"PT2S\" type=\"dynamic\" "
      "profiles=\"urn:mpeg:dash:profile:isoff-live:2011\" "
      "availabilityStartTime=\"2011-12-25T12:30:00\">\n"
      "  <Period start=\"PT0S\"/>\n"
      "</MPD>\n";

  std::string mpd_doc;
  ASSERT_TRUE(mpd_.ToString(&mpd_doc));
  ASSERT_EQ(kExpectedOutput, mpd_doc);
}

// Estimate the bandwidth given the info from AddNewSegment().
TEST_F(SegmentTemplateTest, OneSegmentNormal) {
  const uint64_t kStartTime = 0;
  const uint64_t kDuration = 10;
  const uint64_t kSize = 128;
  AddSegments(kStartTime, kDuration, kSize, 0);

  // TODO(rkuroiwa): Clean up the test/data directory. It's a mess.
  EXPECT_NO_FATAL_FAILURE(CheckMpd(kFileNameExpectedMpdOutputDynamicNormal));
}

TEST_F(SegmentTemplateTest, NormalRepeatedSegmentDuration) {
  const uint64_t kSize = 256;
  uint64_t start_time = 0;
  uint64_t duration = 40000;
  uint64_t repeat = 2;
  AddSegments(start_time, duration, kSize, repeat);

  start_time += duration * (repeat + 1);
  duration = 54321;
  repeat = 0;
  AddSegments(start_time, duration, kSize, repeat);

  start_time += duration * (repeat + 1);
  duration = 12345;
  repeat = 0;
  AddSegments(start_time, duration, kSize, repeat);

  ASSERT_NO_FATAL_FAILURE(CheckMpdAgainstExpectedResult());
}

TEST_F(SegmentTemplateTest, RepeatedSegmentsFromNonZeroStartTime) {
  const uint64_t kSize = 100000;
  uint64_t start_time = 0;
  uint64_t duration = 100000;
  uint64_t repeat = 2;
  AddSegments(start_time, duration, kSize, repeat);

  start_time += duration * (repeat + 1);
  duration = 20000;
  repeat = 3;
  AddSegments(start_time, duration, kSize, repeat);

  start_time += duration * (repeat + 1);
  duration = 32123;
  repeat = 3;
  AddSegments(start_time, duration, kSize, repeat);

  ASSERT_NO_FATAL_FAILURE(CheckMpdAgainstExpectedResult());
}

// Segments not starting from 0.
// Start time is 10. Make sure r gets set correctly.
TEST_F(SegmentTemplateTest, NonZeroStartTime) {
  const uint64_t kStartTime = 10;
  const uint64_t kDuration = 22000;
  const uint64_t kSize = 123456;
  const uint64_t kRepeat = 1;
  AddSegments(kStartTime, kDuration, kSize, kRepeat);

  ASSERT_NO_FATAL_FAILURE(CheckMpdAgainstExpectedResult());
}

// There is a gap in the segments, but still valid.
TEST_F(SegmentTemplateTest, NonContiguousLiveInfo) {
  const uint64_t kStartTime = 10;
  const uint64_t kDuration = 22000;
  const uint64_t kSize = 123456;
  const uint64_t kRepeat = 0;
  AddSegments(kStartTime, kDuration, kSize, kRepeat);

  const uint64_t kStartTimeOffset = 100;
  AddSegments(kDuration + kStartTimeOffset, kDuration, kSize, kRepeat);

  ASSERT_NO_FATAL_FAILURE(CheckMpdAgainstExpectedResult());
}

// Add segments out of order. Segments that start before the previous segment
// cannot be added.
TEST_F(SegmentTemplateTest, OutOfOrder) {
  const uint64_t kEarlierStartTime = 0;
  const uint64_t kLaterStartTime = 1000;
  const uint64_t kDuration = 1000;
  const uint64_t kSize = 123456;
  const uint64_t kRepeat = 0;

  AddSegments(kLaterStartTime, kDuration, kSize, kRepeat);
  AddSegments(kEarlierStartTime, kDuration, kSize, kRepeat);

  ASSERT_NO_FATAL_FAILURE(CheckMpdAgainstExpectedResult());
}

// No segments should be overlapping.
TEST_F(SegmentTemplateTest, OverlappingSegments) {
  const uint64_t kEarlierStartTime = 0;
  const uint64_t kDuration = 1000;
  const uint64_t kSize = 123456;
  const uint64_t kRepeat = 0;

  const uint64_t kOverlappingSegmentStartTime = kDuration / 2;
  CHECK_GT(kDuration, kOverlappingSegmentStartTime);

  AddSegments(kEarlierStartTime, kDuration, kSize, kRepeat);
  AddSegments(kOverlappingSegmentStartTime, kDuration, kSize, kRepeat);

  ASSERT_NO_FATAL_FAILURE(CheckMpdAgainstExpectedResult());
}

// Some segments can be overlapped due to rounding errors. As long as it falls
// in the range of rounding error defined inside MpdBuilder, the segment gets
// accepted.
TEST_F(SegmentTemplateTest, OverlappingSegmentsWithinErrorRange) {
  const uint64_t kEarlierStartTime = 0;
  const uint64_t kDuration = 1000;
  const uint64_t kSize = 123456;
  const uint64_t kRepeat = 0;

  const uint64_t kOverlappingSegmentStartTime = kDuration - 1;
  CHECK_GT(kDuration, kOverlappingSegmentStartTime);

  AddSegments(kEarlierStartTime, kDuration, kSize, kRepeat);
  AddSegments(kOverlappingSegmentStartTime, kDuration, kSize, kRepeat);

  ASSERT_NO_FATAL_FAILURE(CheckMpdAgainstExpectedResult());
}

// All segments have the same duration and size.
TEST_F(TimeShiftBufferDepthTest, Normal) {
  const int kTimeShiftBufferDepth = 10;  // 10 sec.
  mutable_mpd_options()->time_shift_buffer_depth = kTimeShiftBufferDepth;

  const uint64_t kInitialStartTime = 0;
  // Trick to make every segment 1 second long.
  const uint64_t kDuration = DefaultTimeScale();
  const uint64_t kSize = 10000;
  const uint64_t kRepeat = 1234;
  const uint64_t kLength = kRepeat;

  CHECK_EQ(kDuration / DefaultTimeScale() * kRepeat, kLength);

  AddSegments(kInitialStartTime, kDuration, kSize, kRepeat);

  // There should only be the last 11 segments because timeshift is 10 sec and
  // each segment is 1 sec and the latest segments start time is "current
  // time" i.e., the latest segment does not count as part of timeshift buffer
  // depth.
  // Also note that S@r + 1 is the actual number of segments.
  const int kExpectedRepeatsLeft = kTimeShiftBufferDepth;
  const std::string expected_s_element =
      base::StringPrintf(kSElementTemplate,
                         kDuration * (kRepeat - kExpectedRepeatsLeft),
                         kDuration,
                         static_cast<uint64_t>(kExpectedRepeatsLeft));

  const int kExpectedStartNumber = kRepeat - kExpectedRepeatsLeft + 1;
  ASSERT_NO_FATAL_FAILURE(CheckTimeShiftBufferDepthResult(
      expected_s_element, kTimeShiftBufferDepth, kExpectedStartNumber));
}

// TimeShiftBufferDepth is shorter than a segment. This should not discard the
// segment that can play TimeShiftBufferDepth.
// For example if TimeShiftBufferDepth = 1 min. and a 10 min segment was just
// added. Before that 9 min segment was added. The 9 min segment should not be
// removed from the MPD.
TEST_F(TimeShiftBufferDepthTest, TimeShiftBufferDepthShorterThanSegmentLength) {
  const int kTimeShiftBufferDepth = 10;  // 10 sec.
  mutable_mpd_options()->time_shift_buffer_depth = kTimeShiftBufferDepth;

  const uint64_t kInitialStartTime = 0;
  // Each duration is a second longer than timeShiftBufferDepth.
  const uint64_t kDuration = DefaultTimeScale() * (kTimeShiftBufferDepth + 1);
  const uint64_t kSize = 10000;
  const uint64_t kRepeat = 1;

  AddSegments(kInitialStartTime, kDuration, kSize, kRepeat);

  // The two segments should be both present.
  const std::string expected_s_element = base::StringPrintf(
      kSElementTemplate, kInitialStartTime, kDuration, kRepeat);

  ASSERT_NO_FATAL_FAILURE(CheckTimeShiftBufferDepthResult(
      expected_s_element, kTimeShiftBufferDepth, kDefaultStartNumber));
}

// More generic version the normal test.
TEST_F(TimeShiftBufferDepthTest, Generic) {
  const int kTimeShiftBufferDepth = 30;
  mutable_mpd_options()->time_shift_buffer_depth = kTimeShiftBufferDepth;

  const uint64_t kInitialStartTime = 123;
  const uint64_t kDuration = DefaultTimeScale();
  const uint64_t kSize = 10000;
  const uint64_t kRepeat = 1000;

  AddSegments(kInitialStartTime, kDuration, kSize, kRepeat);
  const uint64_t first_s_element_end_time =
      kInitialStartTime + kDuration * (kRepeat + 1);

  // Now add 2 kTimeShiftBufferDepth long segments.
  const int kNumMoreSegments = 2;
  const int kMoreSegmentsRepeat = kNumMoreSegments - 1;
  const uint64_t kTimeShiftBufferDepthDuration =
      DefaultTimeScale() * kTimeShiftBufferDepth;
  AddSegments(first_s_element_end_time,
              kTimeShiftBufferDepthDuration,
              kSize,
              kMoreSegmentsRepeat);

  // Expect only the latest S element with 2 segments.
  const std::string expected_s_element =
      base::StringPrintf(kSElementTemplate,
                         first_s_element_end_time,
                         kTimeShiftBufferDepthDuration,
                         static_cast<uint64_t>(kMoreSegmentsRepeat));

  const int kExpectedRemovedSegments = kRepeat + 1;
  ASSERT_NO_FATAL_FAILURE(CheckTimeShiftBufferDepthResult(
      expected_s_element,
      kTimeShiftBufferDepth,
      kDefaultStartNumber + kExpectedRemovedSegments));
}

// More than 1 S element in the result.
// Adds 100 one-second segments. Then add 21 two-second segments.
// This should have all of the two-second segments and 60 one-second
// segments. Note that it expects 60 segments from the first S element because
// the most recent segment added does not count
TEST_F(TimeShiftBufferDepthTest, MoreThanOneS) {
  const int kTimeShiftBufferDepth = 100;
  mutable_mpd_options()->time_shift_buffer_depth = kTimeShiftBufferDepth;

  const uint64_t kInitialStartTime = 0;
  const uint64_t kSize = 20000;

  const uint64_t kOneSecondDuration = DefaultTimeScale();
  const uint64_t kOneSecondSegmentRepeat = 99;
  AddSegments(
      kInitialStartTime, kOneSecondDuration, kSize, kOneSecondSegmentRepeat);
  const uint64_t first_s_element_end_time =
      kInitialStartTime + kOneSecondDuration * (kOneSecondSegmentRepeat + 1);

  const uint64_t kTwoSecondDuration = 2 * DefaultTimeScale();
  const uint64_t kTwoSecondSegmentRepeat = 20;
  AddSegments(first_s_element_end_time,
              kTwoSecondDuration,
              kSize,
              kTwoSecondSegmentRepeat);

  const uint64_t kExpectedRemovedSegments =
      (kOneSecondSegmentRepeat + 1 + kTwoSecondSegmentRepeat * 2) -
      kTimeShiftBufferDepth;

  std::string expected_s_element =
      base::StringPrintf(kSElementTemplate,
                         kOneSecondDuration * kExpectedRemovedSegments,
                         kOneSecondDuration,
                         kOneSecondSegmentRepeat - kExpectedRemovedSegments);
  expected_s_element += base::StringPrintf(kSElementTemplate,
                                           first_s_element_end_time,
                                           kTwoSecondDuration,
                                           kTwoSecondSegmentRepeat);

  ASSERT_NO_FATAL_FAILURE(CheckTimeShiftBufferDepthResult(
      expected_s_element,
      kTimeShiftBufferDepth,
      kDefaultStartNumber + kExpectedRemovedSegments));
}


// Edge case where the last segment in S element should still be in the MPD.
// Example:
// Assuming timescale = 1 so that duration of 1 means 1 second.
// TimeShiftBufferDepth is 9 sec and we currently have
// <S t=0 d=1.5 r=1 />
// <S t=3 d=2 r=3 />
// and we add another contiguous 2 second segment.
// Then the first S element's last segment should still be in the MPD.
TEST_F(TimeShiftBufferDepthTest, UseLastSegmentInS) {
  const int kTimeShiftBufferDepth = 9;
  mutable_mpd_options()->time_shift_buffer_depth = kTimeShiftBufferDepth;

  const uint64_t kInitialStartTime = 1;
  const uint64_t kDuration1 = static_cast<uint64_t>(DefaultTimeScale() * 1.5);
  const uint64_t kSize = 20000;
  const uint64_t kRepeat1 = 1;

  AddSegments(kInitialStartTime, kDuration1, kSize, kRepeat1);

  const uint64_t first_s_element_end_time =
      kInitialStartTime + kDuration1 * (kRepeat1 + 1);

  const uint64_t kTwoSecondDuration = 2 * DefaultTimeScale();
  const uint64_t kTwoSecondSegmentRepeat = 4;

  AddSegments(first_s_element_end_time,
              kTwoSecondDuration,
              kSize,
              kTwoSecondSegmentRepeat);

  std::string expected_s_element = base::StringPrintf(
      kSElementTemplateWithoutR,
      kInitialStartTime + kDuration1,  // Expect one segment removed.
      kDuration1);

  expected_s_element += base::StringPrintf(kSElementTemplate,
                                           first_s_element_end_time,
                                           kTwoSecondDuration,
                                           kTwoSecondSegmentRepeat);
  ASSERT_NO_FATAL_FAILURE(CheckTimeShiftBufferDepthResult(
      expected_s_element, kTimeShiftBufferDepth, 2));
}

// Gap between S elements but both should be included.
TEST_F(TimeShiftBufferDepthTest, NormalGap) {
  const int kTimeShiftBufferDepth = 10;
  mutable_mpd_options()->time_shift_buffer_depth = kTimeShiftBufferDepth;

  const uint64_t kInitialStartTime = 0;
  const uint64_t kDuration = DefaultTimeScale();
  const uint64_t kSize = 20000;
  const uint64_t kRepeat = 6;
  // CHECK here so that the when next S element is added with 1 segment, this S
  // element doesn't go away.
  CHECK_LT(kRepeat - 1u, static_cast<uint64_t>(kTimeShiftBufferDepth));
  CHECK_EQ(kDuration, DefaultTimeScale());

  AddSegments(kInitialStartTime, kDuration, kSize, kRepeat);

  const uint64_t first_s_element_end_time =
      kInitialStartTime + kDuration * (kRepeat + 1);

  const uint64_t gap_s_element_start_time = first_s_element_end_time + 1;
  AddSegments(gap_s_element_start_time, kDuration, kSize, /* no repeat */ 0);

  std::string expected_s_element = base::StringPrintf(
      kSElementTemplate, kInitialStartTime, kDuration, kRepeat);
  expected_s_element += base::StringPrintf(
      kSElementTemplateWithoutR, gap_s_element_start_time, kDuration);

  ASSERT_NO_FATAL_FAILURE(CheckTimeShiftBufferDepthResult(
      expected_s_element, kTimeShiftBufferDepth, kDefaultStartNumber));
}

// Case where there is a huge gap so the first S element is removed.
TEST_F(TimeShiftBufferDepthTest, HugeGap) {
  const int kTimeShiftBufferDepth = 10;
  mutable_mpd_options()->time_shift_buffer_depth = kTimeShiftBufferDepth;

  const uint64_t kInitialStartTime = 0;
  const uint64_t kDuration = DefaultTimeScale();
  const uint64_t kSize = 20000;
  const uint64_t kRepeat = 6;
  AddSegments(kInitialStartTime, kDuration, kSize, kRepeat);

  const uint64_t first_s_element_end_time =
      kInitialStartTime + kDuration * (kRepeat + 1);

  // Big enough gap so first S element should not be there.
  const uint64_t gap_s_element_start_time =
      first_s_element_end_time +
      (kTimeShiftBufferDepth + 1) * DefaultTimeScale();
  const uint64_t kSecondSElementRepeat = 9;
  COMPILE_ASSERT(
      kSecondSElementRepeat < static_cast<uint64_t>(kTimeShiftBufferDepth),
      second_s_element_repeat_must_be_less_than_time_shift_buffer_depth);
  AddSegments(gap_s_element_start_time, kDuration, kSize, kSecondSElementRepeat);

  std::string expected_s_element = base::StringPrintf(kSElementTemplate,
                                                      gap_s_element_start_time,
                                                      kDuration,
                                                      kSecondSElementRepeat);
  const int kExpectedRemovedSegments = kRepeat + 1;
  ASSERT_NO_FATAL_FAILURE(CheckTimeShiftBufferDepthResult(
      expected_s_element,
      kTimeShiftBufferDepth,
      kDefaultStartNumber + kExpectedRemovedSegments));
}

// Check if startNumber is working correctly.
TEST_F(TimeShiftBufferDepthTest, ManySegments) {
  const int kTimeShiftBufferDepth = 1;
  mutable_mpd_options()->time_shift_buffer_depth = kTimeShiftBufferDepth;

  const uint64_t kInitialStartTime = 0;
  const uint64_t kDuration = DefaultTimeScale();
  const uint64_t kSize = 20000;
  const uint64_t kRepeat = 10000;
  const uint64_t kTotalNumSegments = kRepeat + 1;
  AddSegments(kInitialStartTime, kDuration, kSize, kRepeat);

  const int kExpectedSegmentsLeft = kTimeShiftBufferDepth + 1;
  const int kExpectedSegmentsRepeat = kExpectedSegmentsLeft - 1;
  const int kExpectedRemovedSegments =
      kTotalNumSegments - kExpectedSegmentsLeft;

  std::string expected_s_element =
      base::StringPrintf(kSElementTemplate,
                         kExpectedRemovedSegments * kDuration,
                         kDuration,
                         static_cast<uint64_t>(kExpectedSegmentsRepeat));

  ASSERT_NO_FATAL_FAILURE(CheckTimeShiftBufferDepthResult(
      expected_s_element,
      kTimeShiftBufferDepth,
      kDefaultStartNumber + kExpectedRemovedSegments));
}

TEST(RelativePaths, PathsModified) {
  const std::string kCommonPath(FilePath("foo").Append("bar").value());
  const std::string kMediaFileBase("media.mp4");
  const std::string kInitSegmentBase("init.mp4");
  const std::string kSegmentTemplateBase("segment-$Number$.mp4");
  const std::string kMediaFile(
      FilePath(kCommonPath).Append(kMediaFileBase).value());
  const std::string kInitSegment(
      FilePath(kCommonPath).Append(kInitSegmentBase).value());
  const std::string kSegmentTemplate(
      FilePath(kCommonPath).Append(kSegmentTemplateBase).value());
  const std::string kMpd(FilePath(kCommonPath).Append("media.mpd").value());
  MediaInfo media_info;

  media_info.set_media_file_name(kMediaFile);
  media_info.set_init_segment_name(kInitSegment);
  media_info.set_segment_template(kSegmentTemplate);
  MpdBuilder::MakePathsRelativeToMpd(kMpd, &media_info);
  EXPECT_EQ(kMediaFileBase, media_info.media_file_name());
  EXPECT_EQ(kInitSegmentBase, media_info.init_segment_name());
  EXPECT_EQ(kSegmentTemplateBase, media_info.segment_template());
}

TEST(RelativePaths, PathsNotModified) {
  const std::string kMediaCommon(FilePath("foo").Append("bar").value());
  const std::string kMediaFileBase("media.mp4");
  const std::string kInitSegmentBase("init.mp4");
  const std::string kSegmentTemplateBase("segment-$Number$.mp4");
  const std::string kMediaFile(
      FilePath(kMediaCommon).Append(kMediaFileBase).value());
  const std::string kInitSegment(
      FilePath(kMediaCommon).Append(kInitSegmentBase).value());
  const std::string kSegmentTemplate(
      FilePath(kMediaCommon).Append(kSegmentTemplateBase).value());
  const std::string kMpd(
      FilePath("foo").Append("baz").Append("media.mpd").value());
  MediaInfo media_info;

  media_info.set_media_file_name(kMediaFile);
  media_info.set_init_segment_name(kInitSegment);
  media_info.set_segment_template(kSegmentTemplate);
  MpdBuilder::MakePathsRelativeToMpd(kMpd, &media_info);
  EXPECT_EQ(kMediaFile, media_info.media_file_name());
  EXPECT_EQ(kInitSegment, media_info.init_segment_name());
  EXPECT_EQ(kSegmentTemplate, media_info.segment_template());
}

}  // namespace edash_packager

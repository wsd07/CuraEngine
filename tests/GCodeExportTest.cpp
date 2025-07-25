// Copyright (c) 2023 UltiMaker
// CuraEngine is released under the terms of the AGPLv3 or higher

#include "gcodeExport.h" // The unit under test.

#include <gtest/gtest.h>

#include "Application.h" // To set up a slice with settings.
#include "RetractionConfig.h" // For extruder switch tests.
#include "Slice.h" // To set up a slice with settings.
#include "WipeScriptConfig.h" // For wipe script tests.
#include "arcus/MockCommunication.h" // To prevent calls to any missing Communication class.
#include "utils/Coord_t.h"
#include "utils/Date.h" // To check the Griffin header.

// NOLINTBEGIN(*-magic-numbers)
namespace cura
{

/*
 * Fixture that provides a GCodeExport instance in a certain base state.
 */
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
class GCodeExportTest : public testing::Test
{
public:
    /*
     * An export class to test with.
     */
    GCodeExport gcode;

    /*
     * A stream to capture the output of the g-code export.
     */
    std::stringstream output;

    /*
     * Mock away the communication channel where layer data is output by this
     * class.
     */
    std::shared_ptr<MockCommunication> mock_communication;

    void SetUp() override
    {
        output << std::fixed;
        gcode.output_stream_ = &output;

        // Since GCodeExport doesn't support copying, we have to reset everything in-place.
        gcode.current_position_ = Point3LL(0, 0, MM2INT(20));
        gcode.layer_nr_ = 0;
        gcode.current_e_value_ = 0;
        gcode.current_e_offset_ = 0;
        gcode.current_extruder_ = 0;
        gcode.total_print_times_ = std::vector<Duration>(static_cast<unsigned char>(PrintFeatureType::NumPrintFeatureTypes), 0.0);
        gcode.current_speed_ = 1.0;
        gcode.current_print_acceleration_ = -1.0;
        gcode.current_travel_acceleration_ = -1.0;
        gcode.current_jerk_ = -1.0;
        gcode.is_z_hopped_ = 0;
        gcode.setFlavor(EGCodeFlavor::MARLIN);
        gcode.bed_temperature_ = 0;
        gcode.initial_bed_temp_ = 0;
        gcode.total_bounding_box_ = AABB3D();
        gcode.current_layer_z_ = 0;
        gcode.relative_extrusion_ = false;

        gcode.new_line_ = "\n"; // Not BFB flavour by default.
        gcode.machine_name_ = "Your favourite 3D printer";

        // Set up a scene so that we may request settings.
        Application::getInstance().current_slice_ = std::make_shared<Slice>(1);
        mock_communication = std::make_shared<MockCommunication>();
        Application::getInstance().communication_ = mock_communication;
    }

    void TearDown() override
    {
        Application::getInstance().communication_ = nullptr;
    }
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

TEST_F(GCodeExportTest, CommentEmpty)
{
    gcode.writeComment("");
    EXPECT_EQ(std::string(";\n"), output.str()) << "Semicolon and newline must exist but it must be empty for the rest.";
}

TEST_F(GCodeExportTest, CommentSimple)
{
    gcode.writeComment("extrude harder");
    EXPECT_EQ(std::string(";extrude harder\n"), output.str()) << "Message must be preceded by a semicolon and ends with a newline..";
}

TEST_F(GCodeExportTest, CommentMultiLine)
{
    gcode.writeComment(
        "If you catch a chinchilla in Chile\n"
        "And cut off its beard, willy-nilly\n"
        "You can honestly say\n"
        "You made on that day\n"
        "A Chilean chinchilla's chin chilly");
    EXPECT_EQ(
        std::string(
            ";If you catch a chinchilla in Chile\n"
            ";And cut off its beard, willy-nilly\n"
            ";You can honestly say\n"
            ";You made on that day\n"
            ";A Chilean chinchilla's chin chilly\n"),
        output.str())
        << "Each line must be preceded by a semicolon.";
}

TEST_F(GCodeExportTest, CommentMultiple)
{
    gcode.writeComment("Thunderbolt and lightning");
    gcode.writeComment("Very very frightening me");
    gcode.writeComment(" - Galileo (1638)");
    EXPECT_EQ(
        std::string(
            ";Thunderbolt and lightning\n"
            ";Very very frightening me\n"
            "; - Galileo (1638)\n"),
        output.str())
        << "Semicolon before each line, and newline in between.";
}

TEST_F(GCodeExportTest, CommentTimeZero)
{
    gcode.writeTimeComment(0);
    EXPECT_EQ(std::string(";TIME_ELAPSED:0.000000\n"), output.str());
}

TEST_F(GCodeExportTest, CommentTimeInteger)
{
    gcode.writeTimeComment(42);
    EXPECT_EQ(std::string(";TIME_ELAPSED:42.000000\n"), output.str()) << "The time must be fixed-radix to the microsecond.";
}

TEST_F(GCodeExportTest, CommentTimeFloatRoundingError)
{
    gcode.writeTimeComment(0.3);
    EXPECT_EQ(std::string(";TIME_ELAPSED:0.300000\n"), output.str()) << "Don't output up to the precision of rounding errors.";
}

TEST_F(GCodeExportTest, CommentTypeAllTypesCovered)
{
    for (auto type = static_cast<PrintFeatureType>(0); type < PrintFeatureType::NumPrintFeatureTypes; type = static_cast<PrintFeatureType>(static_cast<size_t>(type) + 1))
    {
        gcode.writeTypeComment(type);
        if (type == PrintFeatureType::MoveUnretracted || type == PrintFeatureType::MoveRetracted || type == PrintFeatureType::MoveWhileRetracting
            || type == PrintFeatureType::MoveWhileUnretracting || type == PrintFeatureType::StationaryRetractUnretract)
        {
            EXPECT_EQ(std::string(""), output.str()) << "Travel moves shouldn't output a type.";
        }
        else if (type == PrintFeatureType::NoneType)
        {
            EXPECT_EQ(std::string(""), output.str()) << "NoneType shouldn't output a type.";
        }
        else
        {
            EXPECT_EQ(std::string(";TYPE:"), output.str().substr(0, 6)) << "Type " << static_cast<size_t>(type) << " is not implemented.";
        }
        output.str(""); // Reset so that our next measurement is clean again.
        output << std::fixed;
    }
}

TEST_F(GCodeExportTest, CommentLayer)
{
    gcode.writeLayerComment(9);
    EXPECT_EQ(std::string(";LAYER:9\n"), output.str()) << "Put the correct prefix and a newline afterwards.";
}

TEST_F(GCodeExportTest, CommentLayerNegative)
{
    gcode.writeLayerComment(-3);
    EXPECT_EQ(std::string(";LAYER:-3\n"), output.str());
}

TEST_F(GCodeExportTest, CommentLayerCount)
{
    gcode.writeLayerCountComment(5);
    EXPECT_EQ(std::string(";LAYER_COUNT:5\n"), output.str());
}

/*
 * Parameterized test with different numbers of extruders.
 */
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
class GriffinHeaderTest : public testing::TestWithParam<size_t>
{
public:
    /*
     * An export class to test with.
     */
    GCodeExport gcode;

    /*
     * A stream to capture the output of the g-code export.
     */
    std::stringstream output;

    void SetUp() override
    {
        output << std::fixed;
        gcode.output_stream_ = &output;

        // Since GCodeExport doesn't support copying, we have to reset everything in-place.
        gcode.current_position_ = Point3LL(0, 0, MM2INT(20));
        gcode.layer_nr_ = 0;
        gcode.current_e_value_ = 0;
        gcode.current_extruder_ = 0;
        gcode.total_print_times_ = std::vector<Duration>(static_cast<unsigned char>(PrintFeatureType::NumPrintFeatureTypes), 0.0);
        gcode.current_speed_ = 1.0;
        gcode.current_print_acceleration_ = -1.0;
        gcode.current_travel_acceleration_ = -1.0;
        gcode.current_jerk_ = -1.0;
        gcode.is_z_hopped_ = 0;
        gcode.setFlavor(EGCodeFlavor::MARLIN);
        gcode.initial_bed_temp_ = 0;
        gcode.bed_temperature_ = 0;
        gcode.total_bounding_box_ = AABB3D();

        gcode.new_line_ = "\n"; // Not BFB flavour by default.
        gcode.machine_name_ = "Your favourite 3D printer";

        // Set up a scene so that we may request settings.
        Application::getInstance().current_slice_ = std::make_shared<Slice>(0);
    }
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

TEST_P(GriffinHeaderTest, HeaderGriffinFormat)
{
    const size_t num_extruders = GetParam();
    gcode.flavor_ = EGCodeFlavor::GRIFFIN;
    for (size_t extruder_index = 0; extruder_index < num_extruders; extruder_index++)
    {
        Application::getInstance().current_slice_->scene.extruders.emplace_back(extruder_index, nullptr);
        ExtruderTrain& train = Application::getInstance().current_slice_->scene.extruders.back();
        train.settings_.add("machine_nozzle_size", "0.4");
        train.settings_.add("machine_nozzle_id", "TestNozzle");
    }

    const std::vector<bool> extruder_is_used(num_extruders, true);
    std::istringstream result(gcode.getFileHeader(extruder_is_used));
    std::string token;

    std::getline(result, token, '\n');
    EXPECT_EQ(std::string(";START_OF_HEADER"), token);
    std::getline(result, token, '\n');
    EXPECT_EQ(std::string(";HEADER_VERSION:"), token.substr(0, 16)); // Actual version doesn't matter in this test.
    std::getline(result, token, '\n');
    EXPECT_EQ(std::string(";FLAVOR:Griffin"), token);
    std::getline(result, token, '\n');
    EXPECT_EQ(std::string(";GENERATOR.NAME:Cura_SteamEngine"), token);
    std::getline(result, token, '\n');
    EXPECT_EQ(std::string(";GENERATOR.VERSION:"), token.substr(0, 19));
    EXPECT_EQ(std::string(CURA_ENGINE_VERSION), token.substr(19));
    std::getline(result, token, '\n');
    EXPECT_EQ(std::string(";GENERATOR.BUILD_DATE:"), token.substr(0, 22));
    EXPECT_EQ(Date::getDate().toStringDashed(), token.substr(22));
    std::getline(result, token, '\n');
    EXPECT_EQ(std::string(";TARGET_MACHINE.NAME:"), token.substr(0, 21));
    EXPECT_EQ(gcode.machine_name_, token.substr(21));

    for (size_t extruder_nr = 0; extruder_nr < num_extruders; extruder_nr++)
    {
        std::getline(result, token, '\n');
        EXPECT_EQ(std::string(";EXTRUDER_TRAIN."), token.substr(0, 16));
        EXPECT_EQ(std::to_string(extruder_nr), token.substr(16, 1)); // TODO: Assumes the extruder nr is 1 digit.
        EXPECT_EQ(std::string(".INITIAL_TEMPERATURE:"), token.substr(17, 21)); // Actual temperature doesn't matter.
        std::getline(result, token, '\n');
        EXPECT_EQ(std::string(";EXTRUDER_TRAIN."), token.substr(0, 16));
        EXPECT_EQ(std::to_string(extruder_nr), token.substr(16, 1)); // TODO: Assumes the extruder nr is 1 digit.
        EXPECT_EQ(std::string(".NOZZLE.DIAMETER:0.4"), token.substr(17, 20)); // Nozzle size needs to be equal to the machine_nozzle_size setting.
        std::getline(result, token, '\n');
        EXPECT_EQ(std::string(";EXTRUDER_TRAIN."), token.substr(0, 16));
        EXPECT_EQ(std::to_string(extruder_nr), token.substr(16, 1)); // TODO: Assumes the extruder nr is 1 digit.
        EXPECT_EQ(std::string(".NOZZLE.NAME:TestNozzle"), token.substr(17, 23)); // Nozzle name needs to be equal to the machine_nozzle_id setting.
    }

    std::getline(result, token, '\n');
    EXPECT_EQ(std::string(";BUILD_PLATE.INITIAL_TEMPERATURE:"), token.substr(0, 33)); // Actual temperature doesn't matter in this test.
    std::getline(result, token, '\n');
    EXPECT_EQ(std::string(";PRINT.GROUPS:0"), token);
    std::getline(result, token, '\n');
    EXPECT_EQ(std::string(";PRINT.SIZE.MIN.X:"), token.substr(0, 18)); // Actual bounds don't matter in this test.
    std::getline(result, token, '\n');
    EXPECT_EQ(std::string(";PRINT.SIZE.MIN.Y:"), token.substr(0, 18));
    std::getline(result, token, '\n');
    EXPECT_EQ(std::string(";PRINT.SIZE.MIN.Z:"), token.substr(0, 18));
    std::getline(result, token, '\n');
    EXPECT_EQ(std::string(";PRINT.SIZE.MAX.X:"), token.substr(0, 18));
    std::getline(result, token, '\n');
    EXPECT_EQ(std::string(";PRINT.SIZE.MAX.Y:"), token.substr(0, 18));
    std::getline(result, token, '\n');
    EXPECT_EQ(std::string(";PRINT.SIZE.MAX.Z:"), token.substr(0, 18));
    std::getline(result, token, '\n');
    EXPECT_EQ(std::string(";SLICE_UUID:"), token.substr(0, 18));
    std::getline(result, token, '\n');
    EXPECT_EQ(std::string(";END_OF_HEADER"), token);
}

INSTANTIATE_TEST_SUITE_P(GriffinHeaderTestInstantiation, GriffinHeaderTest, testing::Values(0, 1, 2, 9));

/*
 * Test the default header generation.
 */
TEST_F(GCodeExportTest, HeaderUltiGCode)
{
    gcode.flavor_ = EGCodeFlavor::ULTIGCODE;
    constexpr size_t num_extruders = 2;
    const std::vector<bool> extruder_is_used(num_extruders, true);
    constexpr Duration print_time = 1337;
    const std::vector<double> filament_used = { 100, 200 };
    for (size_t extruder_index = 0; extruder_index < num_extruders; extruder_index++)
    {
        Application::getInstance().current_slice_->scene.extruders.emplace_back(extruder_index, nullptr);
        ExtruderTrain& train = Application::getInstance().current_slice_->scene.extruders.back();
        train.settings_.add("machine_nozzle_size", "0.4");
    }
    gcode.total_bounding_box_ = AABB3D(Point3LL(0, 0, 0), Point3LL(1000, 1000, 1000));

    std::string result = gcode.getFileHeader(extruder_is_used, &print_time, filament_used);

    EXPECT_EQ(
        result,
        ";FLAVOR:UltiGCode\n;TIME:1337\n;MATERIAL:100\n;MATERIAL2:200\n;NOZZLE_DIAMETER:0.4\n;MINX:0\n;MINY:0\n;MINZ:0\n;MAXX:1\n;"
        "MAXY:1\n;MAXZ:1\n;TARGET_MACHINE.NAME:Your favourite 3D printer\n");
}

TEST_F(GCodeExportTest, HeaderRepRap)
{
    Application::getInstance().current_slice_->scene.current_mesh_group->settings.add("layer_height", "0.123");
    gcode.flavor_ = EGCodeFlavor::REPRAP;
    gcode.extruder_attr_[0].filament_area_ = 5.0;
    gcode.extruder_attr_[1].filament_area_ = 4.0;
    constexpr size_t num_extruders = 2;
    const std::vector<bool> extruder_is_used(num_extruders, true);
    constexpr Duration print_time = 1337;
    const std::vector<double> filament_used = { 100, 200 };
    gcode.total_bounding_box_ = AABB3D(Point3LL(0, 0, 0), Point3LL(1000, 1000, 1000));

    std::string result = gcode.getFileHeader(extruder_is_used, &print_time, filament_used);

    EXPECT_EQ(
        result,
        ";FLAVOR:RepRap\n;TIME:1337\n;Filament used: 0.02m, 0.05m\n;Layer height: "
        "0.123\n;MINX:0\n;MINY:0\n;MINZ:0\n;MAXX:1\n;MAXY:1\n;MAXZ:1\n;TARGET_MACHINE.NAME:Your favourite 3D printer\n");
}

TEST_F(GCodeExportTest, HeaderMarlin)
{
    Application::getInstance().current_slice_->scene.current_mesh_group->settings.add("layer_height", "0.123");
    gcode.flavor_ = EGCodeFlavor::MARLIN;
    gcode.extruder_attr_[0].filament_area_ = 5.0;
    gcode.extruder_attr_[1].filament_area_ = 4.0;
    constexpr size_t num_extruders = 2;
    const std::vector<bool> extruder_is_used(num_extruders, true);
    constexpr Duration print_time = 1337;
    const std::vector<double> filament_used = { 100, 200 };
    gcode.total_bounding_box_ = AABB3D(Point3LL(0, 0, 0), Point3LL(1000, 1000, 1000));

    std::string result = gcode.getFileHeader(extruder_is_used, &print_time, filament_used);

    EXPECT_EQ(
        result,
        ";FLAVOR:Marlin\n;TIME:1337\n;Filament used: 0.02m, 0.05m\n;Layer height: "
        "0.123\n;MINX:0\n;MINY:0\n;MINZ:0\n;MAXX:1\n;MAXY:1\n;MAXZ:1\n;TARGET_MACHINE.NAME:Your favourite 3D printer\n");
}

TEST_F(GCodeExportTest, HeaderMarlinVolumetric)
{
    Application::getInstance().current_slice_->scene.current_mesh_group->settings.add("layer_height", "0.123");
    gcode.flavor_ = EGCodeFlavor::MARLIN_VOLUMATRIC;
    constexpr size_t num_extruders = 2;
    const std::vector<bool> extruder_is_used(num_extruders, true);
    constexpr Duration print_time = 1337;
    const std::vector<double> filament_used = { 100, 200 };
    gcode.total_bounding_box_ = AABB3D(Point3LL(0, 0, 0), Point3LL(1000, 1000, 1000));

    std::string result = gcode.getFileHeader(extruder_is_used, &print_time, filament_used);

    EXPECT_EQ(
        result,
        ";FLAVOR:Marlin(Volumetric)\n;TIME:1337\n;Filament used: 100mm3, 200mm3\n;Layer height: "
        "0.123\n;MINX:0\n;MINY:0\n;MINZ:0\n;MAXX:1\n;MAXY:1\n;MAXZ:1\n;TARGET_MACHINE.NAME:Your favourite 3D printer\n");
}

/*
 * Test conversion from E values to millimetres and back in the case of a
 * volumetric printer.
 */
TEST_F(GCodeExportTest, EVsMmVolumetric)
{
    constexpr double filament_area = 10.0;
    gcode.extruder_attr_[0].filament_area_ = filament_area;
    gcode.is_volumetric_ = true;

    constexpr double mm3_input = 15.0;
    EXPECT_EQ(gcode.mm3ToE(mm3_input), mm3_input) << "Since the E is volumetric and the input mm3 is also volumetric, the output needs to be the same.";

    EXPECT_EQ(gcode.eToMm(200.0), 200.0 / filament_area) << "Since the E is volumetric but mm is linear, divide by the cross-sectional "
                                                            "area of the filament to convert the volume to a length.";

    constexpr double mm_input = 33.0;
    EXPECT_EQ(gcode.mmToE(mm_input), mm_input * filament_area)
        << "Since the input mm is linear but the E output must be volumetric, we need to multiply by the cross-sectional area to convert "
           "length to volume.";

    constexpr double e_input = 100.0;
    EXPECT_EQ(gcode.eToMm3(e_input, 0), e_input) << "Since the E is volumetric and mm3 is also volumetric, the output needs to be the same.";
}

/*
 * Test conversion from E values to millimetres and back in the case where the E
 * value represents the linear position of the filament.
 */
TEST_F(GCodeExportTest, EVsMmLinear)
{
    constexpr double filament_area = 10.0;
    gcode.extruder_attr_[0].filament_area_ = filament_area;
    gcode.is_volumetric_ = false;

    EXPECT_EQ(gcode.mmToE(15.0), 15.0) << "Since the E is linear and the input mm is also linear, the output needs to be the same.";
    EXPECT_EQ(gcode.eToMm(15.0), 15.0) << "Since the E is linear and the output mm is also linear, the output needs to be the same.";

    for (int x = -1000; x < 1000; x += 16)
    {
        EXPECT_DOUBLE_EQ(gcode.mmToE(gcode.eToMm(static_cast<double>(x))), static_cast<double>(x)) << "Converting back and forth should lead to the same number.";
    }

    constexpr double mm3_input = 33.0;
    EXPECT_EQ(gcode.mm3ToE(mm3_input), mm3_input / filament_area)
        << "Since the input mm3 is volumetric but the E output must be linear, we need to divide by the cross-sectional area to convert "
           "volume to length.";

    constexpr double e_input = 100.0;
    EXPECT_EQ(gcode.eToMm3(e_input, 0), e_input * filament_area) << "Since the input E is linear but the output must be volumetric, we "
                                                                    "need to multiply by cross-sectional area to convert length to volume.";
}

/*
 * Switch extruders, with the following special cases:
 * - No retraction distance.
 */
TEST_F(GCodeExportTest, SwitchExtruderSimple)
{
    Scene& scene = Application::getInstance().current_slice_->scene;

    scene.extruders.emplace_back(0, nullptr);
    ExtruderTrain& train1 = scene.extruders.back();

    train1.settings_.add("machine_extruder_prestart_code", ";PRESTART FIRST EXTRUDER");
    train1.settings_.add("machine_extruder_change_duration", "10.0");
    train1.settings_.add("machine_extruder_start_code", ";FIRST EXTRUDER START G-CODE!");
    train1.settings_.add("machine_extruder_end_code", ";FIRST EXTRUDER END G-CODE!");
    train1.settings_.add("machine_extruder_start_code_duration", "0.0");
    train1.settings_.add("machine_extruder_end_code_duration", "0.0");
    train1.settings_.add("machine_firmware_retract", "True");
    train1.settings_.add("retraction_enable", "True");

    scene.extruders.emplace_back(1, nullptr);
    ExtruderTrain& train2 = scene.extruders.back();

    train2.settings_.add("machine_extruder_prestart_code", ";PRESTART SECOND EXTRUDER");
    train2.settings_.add("machine_extruder_change_duration", "11.1");
    train2.settings_.add("machine_extruder_start_code", ";SECOND EXTRUDER START G-CODE!");
    train2.settings_.add("machine_extruder_end_code", ";SECOND EXTRUDER END G-CODE!");
    train2.settings_.add("machine_extruder_start_code_duration", "0.0");
    train2.settings_.add("machine_extruder_end_code_duration", "0.0");
    train2.settings_.add("machine_firmware_retract", "True");
    train2.settings_.add("retraction_enable", "True");

    RetractionConfig no_retraction;
    no_retraction.distance = 0;

    EXPECT_CALL(*mock_communication, setExtruderForSend(testing::_));
    EXPECT_CALL(*mock_communication, sendCurrentPosition(testing::_));
    gcode.switchExtruder(1, no_retraction);

    EXPECT_EQ(std::string("G92 E0\n;FIRST EXTRUDER END G-CODE!\n;PRESTART SECOND EXTRUDER\nT1\nG92 E0\n;SECOND EXTRUDER START G-CODE!\n"), output.str());
}

TEST_F(GCodeExportTest, WriteZHopStartZero)
{
    gcode.writeZhopStart(0);
    EXPECT_EQ(std::string(""), output.str()) << "Zero length z hop shouldn't affect gcode output.";
}

TEST_F(GCodeExportTest, WriteZHopStartDefaultSpeed)
{
    Application::getInstance().current_slice_->scene.extruders.emplace_back(0, nullptr);
    Application::getInstance().current_slice_->scene.extruders[gcode.current_extruder_].settings_.add("speed_z_hop", "1"); // 60mm/min.
    Application::getInstance().current_slice_->scene.current_mesh_group->settings.add("layer_height", "0.2");
    gcode.current_layer_z_ = 2000;
    constexpr coord_t hop_height = 3000;
    gcode.writeZhopStart(hop_height);
    EXPECT_EQ(std::string("G1 F60 Z5\n"), output.str());
}

TEST_F(GCodeExportTest, WriteZHopStartCustomSpeed)
{
    Application::getInstance().current_slice_->scene.extruders.emplace_back(0, nullptr);
    Application::getInstance().current_slice_->scene.extruders[gcode.current_extruder_].settings_.add("speed_z_hop", "1"); // 60mm/min.
    Application::getInstance().current_slice_->scene.current_mesh_group->settings.add("layer_height", "0.2");
    gcode.current_layer_z_ = 2000;
    constexpr coord_t hop_height = 3000;
    constexpr Velocity speed{ 4.0 }; // 240 mm/min.
    gcode.writeZhopStart(hop_height, speed);
    EXPECT_EQ(std::string("G1 F240 Z5\n"), output.str()) << "Custom provided speed should be used.";
}

TEST_F(GCodeExportTest, WriteZHopEndZero)
{
    gcode.is_z_hopped_ = 0;
    gcode.writeZhopEnd();
    EXPECT_EQ(std::string(""), output.str()) << "Zero length z hop shouldn't affect gcode output.";
}

TEST_F(GCodeExportTest, WriteZHopEndDefaultSpeed)
{
    Application::getInstance().current_slice_->scene.extruders.emplace_back(0, nullptr);
    Application::getInstance().current_slice_->scene.extruders[gcode.current_extruder_].settings_.add("speed_z_hop", "1"); // 60mm/min.
    Application::getInstance().current_slice_->scene.current_mesh_group->settings.add("layer_height", "0.2");
    gcode.current_layer_z_ = 2000;
    gcode.is_z_hopped_ = 3000;
    gcode.writeZhopEnd();
    EXPECT_EQ(std::string("G1 F60 Z2\n"), output.str());
}

TEST_F(GCodeExportTest, WriteZHopEndCustomSpeed)
{
    Application::getInstance().current_slice_->scene.extruders.emplace_back(0, nullptr);
    Application::getInstance().current_slice_->scene.extruders[gcode.current_extruder_].settings_.add("speed_z_hop", "1");
    Application::getInstance().current_slice_->scene.current_mesh_group->settings.add("layer_height", "0.2");
    gcode.current_layer_z_ = 2000;
    gcode.is_z_hopped_ = 3000;
    constexpr Velocity speed{ 4.0 }; // 240 mm/min.
    gcode.writeZhopEnd(speed);
    EXPECT_EQ(std::string("G1 F240 Z2\n"), output.str()) << "Custom provided speed should be used.";
}

TEST_F(GCodeExportTest, insertWipeScriptSingleMove)
{
    gcode.current_position_ = Point3LL(1000, 1000, 1000);
    gcode.current_layer_z_ = 1000;
    gcode.use_extruder_offset_to_offset_coords_ = false;
    Application::getInstance().current_slice_->scene.current_mesh_group->settings.add("layer_height", "0.2");

    WipeScriptConfig config;
    config.retraction_enable = false;
    config.hop_enable = false;
    config.brush_pos_x = 2000;
    config.repeat_count = 1;
    config.move_distance = 500;
    config.move_speed = 10.0;
    config.pause = 0;

    EXPECT_CALL(*mock_communication, sendLineTo(testing::_, testing::_, testing::_, testing::_, testing::_)).Times(3);
    gcode.insertWipeScript(config);

    std::string token;
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string(";WIPE_SCRIPT_BEGIN"), token) << "Wipe script should always start with tag.";
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string("G0 F600 X2 Y1"), token) << "Wipe script should go to its position.";
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string("G0 X2.5 Y1"), token) << "There should be one wipe move.";
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string("G0 X1 Y1"), token) << "Wipe script should return back to position before wipe.";
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string(";WIPE_SCRIPT_END"), token) << "Wipe script should always end with tag.";
}

TEST_F(GCodeExportTest, insertWipeScriptMultipleMoves)
{
    gcode.current_position_ = Point3LL(1000, 1000, 1000);
    gcode.current_layer_z_ = 1000;
    gcode.use_extruder_offset_to_offset_coords_ = false;
    Application::getInstance().current_slice_->scene.current_mesh_group->settings.add("layer_height", "0.2");

    WipeScriptConfig config;
    config.retraction_enable = false;
    config.hop_enable = false;
    config.brush_pos_x = 2000;
    config.repeat_count = 4;
    config.move_distance = 500;
    config.move_speed = 10.0;
    config.pause = 0;

    EXPECT_CALL(*mock_communication, sendLineTo(testing::_, testing::_, testing::_, testing::_, testing::_)).Times(6);
    gcode.insertWipeScript(config);

    std::string token;
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string(";WIPE_SCRIPT_BEGIN"), token) << "Wipe script should always start with tag.";
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string("G0 F600 X2 Y1"), token) << "Wipe script should go to its position.";
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string("G0 X2.5 Y1"), token);
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string("G0 X2 Y1"), token);
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string("G0 X2.5 Y1"), token);
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string("G0 X2 Y1"), token);
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string("G0 X1 Y1"), token) << "Wipe script should return back to position before wipe.";
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string(";WIPE_SCRIPT_END"), token) << "Wipe script should always end with tag.";
}

TEST_F(GCodeExportTest, insertWipeScriptOptionalDelay)
{
    gcode.current_position_ = Point3LL(1000, 1000, 1000);
    gcode.current_layer_z_ = 1000;
    gcode.use_extruder_offset_to_offset_coords_ = false;
    Application::getInstance().current_slice_->scene.current_mesh_group->settings.add("layer_height", "0.2");

    WipeScriptConfig config;
    config.retraction_enable = false;
    config.hop_enable = false;
    config.brush_pos_x = 2000;
    config.repeat_count = 1;
    config.move_distance = 500;
    config.move_speed = 10.0;
    config.pause = 1.5; // 1.5 sec = 1500 ms.

    EXPECT_CALL(*mock_communication, sendLineTo(testing::_, testing::_, testing::_, testing::_, testing::_)).Times(3);
    gcode.insertWipeScript(config);

    std::string token;
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string(";WIPE_SCRIPT_BEGIN"), token) << "Wipe script should always start with tag.";
    std::getline(output, token, '\n'); // go to wipe position
    std::getline(output, token, '\n'); // make wipe move
    std::getline(output, token, '\n'); // return back
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string("G4 P1500"), token) << "Wipe script should make a delay.";
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string(";WIPE_SCRIPT_END"), token) << "Wipe script should always end with tag.";
}

TEST_F(GCodeExportTest, insertWipeScriptRetractionEnable)
{
    gcode.current_position_ = Point3LL(1000, 1000, 1000);
    gcode.current_layer_z_ = 1000;
    gcode.current_e_value_ = 100;
    gcode.use_extruder_offset_to_offset_coords_ = false;
    gcode.is_volumetric_ = false;
    gcode.current_extruder_ = 0;
    gcode.extruder_attr_[0].filament_area_ = 10.0;
    gcode.extruder_attr_[0].machine_firmware_retract_ = false;
    gcode.relative_extrusion_ = false;
    gcode.current_speed_ = 1.0;
    Application::getInstance().current_slice_->scene.current_mesh_group->settings.add("layer_height", "0.2");

    WipeScriptConfig config;
    config.retraction_enable = true;
    config.retraction_config.distance = 1;
    config.retraction_config.speed = 2.0; // 120 mm/min.
    config.retraction_config.primeSpeed = 3.0; // 180 mm/min.
    config.retraction_config.prime_volume = gcode.extruder_attr_[0].filament_area_ * 4; // 4mm in linear dimensions
    config.retraction_config.retraction_count_max = 100; // Practically no limit.
    config.retraction_config.retraction_extrusion_window = 1;
    config.retraction_config.retraction_min_travel_distance = 0; // Don't limit retractions for being too short.
    config.hop_enable = false;
    config.brush_pos_x = 2000;
    config.repeat_count = 1;
    config.move_distance = 500;
    config.move_speed = 10.0;
    config.pause = 0;

    EXPECT_CALL(*mock_communication, sendLineTo(testing::_, testing::_, testing::_, testing::_, testing::_)).Times(3);
    gcode.insertWipeScript(config);

    std::string token;
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string(";WIPE_SCRIPT_BEGIN"), token) << "Wipe script should always start with tag.";
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string("G1 F120 E99"), token) << "Wipe script should perform retraction with provided speed and retraction distance.";
    std::getline(output, token, '\n'); // go to wipe position
    std::getline(output, token, '\n'); // make wipe move
    std::getline(output, token, '\n'); // return back
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string("G1 F180 E104"), token) << "Wipe script should make unretraction with provided speed and extra prime volume.";
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string(";WIPE_SCRIPT_END"), token) << "Wipe script should always end with tag.";
}

TEST_F(GCodeExportTest, insertWipeScriptHopEnable)
{
    gcode.current_position_ = Point3LL(1000, 1000, 1000);
    gcode.current_layer_z_ = 1000;
    gcode.use_extruder_offset_to_offset_coords_ = false;
    gcode.current_speed_ = 1.0;
    Application::getInstance().current_slice_->scene.current_mesh_group->settings.add("layer_height", "0.2");

    WipeScriptConfig config;
    config.retraction_enable = false;
    config.hop_enable = true;
    config.hop_speed = 2.0; // 120 mm/min.
    config.hop_amount = 300;
    config.brush_pos_x = 2000;
    config.repeat_count = 1;
    config.move_distance = 500;
    config.move_speed = 10.0;
    config.pause = 0;

    EXPECT_CALL(*mock_communication, sendLineTo(testing::_, testing::_, testing::_, testing::_, testing::_)).Times(5);
    gcode.insertWipeScript(config);

    std::string token;
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string(";WIPE_SCRIPT_BEGIN"), token) << "Wipe script should always start with tag.";
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string("G1 F120 Z1.3"), token) << "Wipe script should perform z-hop.";
    std::getline(output, token, '\n'); // go to wipe position
    std::getline(output, token, '\n'); // make wipe move
    std::getline(output, token, '\n'); // return back
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string("G1 F120 Z1"), token) << "Wipe script should return z position.";
    std::getline(output, token, '\n');
    EXPECT_EQ(std::string(";WIPE_SCRIPT_END"), token) << "Wipe script should always end with tag.";
}
} // namespace cura
// NOLINTEND(*-magic-numbers)

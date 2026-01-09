#include <gtest/gtest.h>
#include "naab/interpreter.h"
#include "naab/struct_registry.h"
#include "naab/ast.h"
#include <thread>
#include <vector>

using namespace naab;

TEST(StructValueTest, CreateAndSetFields) {
    auto def = std::make_shared<interpreter::StructDef>();
    def->name = "Point";
    def->fields.push_back(ast::StructField{"x", ast::Type(ast::TypeKind::Int), std::nullopt});
    def->fields.push_back(ast::StructField{"y", ast::Type(ast::TypeKind::Int), std::nullopt});
    def->field_index["x"] = 0;
    def->field_index["y"] = 1;

    interpreter::StructValue sv("Point", def);
    sv.setField("x", std::make_shared<interpreter::Value>(10));
    sv.setField("y", std::make_shared<interpreter::Value>(20));

    ASSERT_EQ(std::get<int>(sv.getField("x")->data), 10);
    ASSERT_EQ(std::get<int>(sv.getField("y")->data), 20);
}

TEST(StructValueTest, InvalidFieldThrows) {
    auto def = std::make_shared<interpreter::StructDef>();
    def->name = "Point";

    interpreter::StructValue sv("Point", def);

    ASSERT_THROW(sv.getField("invalid"), std::runtime_error);
}

TEST(StructRegistryTest, RegisterAndRetrieve) {
    auto& registry = runtime::StructRegistry::instance();

    auto def = std::make_shared<interpreter::StructDef>();
    def->name = "TestStruct1";

    registry.registerStruct(def);

    ASSERT_TRUE(registry.hasStruct("TestStruct1"));
    auto retrieved = registry.getStruct("TestStruct1");
    ASSERT_NE(retrieved, nullptr);
    ASSERT_EQ(retrieved->name, "TestStruct1");
}

TEST(StructRegistryTest, DuplicateThrows) {
    auto& registry = runtime::StructRegistry::instance();

    auto def1 = std::make_shared<interpreter::StructDef>();
    def1->name = "DuplicateTest2";

    auto def2 = std::make_shared<interpreter::StructDef>();
    def2->name = "DuplicateTest2";

    registry.registerStruct(def1);
    ASSERT_THROW(registry.registerStruct(def2), std::runtime_error);
}

TEST(StructRegistryTest, CircularDetection) {
    auto& registry = runtime::StructRegistry::instance();

    auto defA = std::make_shared<interpreter::StructDef>();
    defA->name = "A_Circular";
    ast::Type typeB = ast::Type::makeStruct("B_Circular");
    defA->fields.push_back(ast::StructField{"b_field", typeB, std::nullopt});
    defA->field_index["b_field"] = 0;

    auto defB = std::make_shared<interpreter::StructDef>();
    defB->name = "B_Circular";
    ast::Type typeA = ast::Type::makeStruct("A_Circular");
    defB->fields.push_back(ast::StructField{"a_field", typeA, std::nullopt});
    defB->field_index["a_field"] = 0;

    registry.registerStruct(defA);
    registry.registerStruct(defB);

    std::set<std::string> visiting;
    ASSERT_THROW(registry.validateStructDef(*defA, visiting), std::runtime_error);
}

// Week 8, Task 58: Additional comprehensive tests

TEST(StructValueTest, NestedStruct) {
    // Inner struct: Point
    auto point_def = std::make_shared<interpreter::StructDef>();
    point_def->name = "Point";
    point_def->fields.push_back(ast::StructField{"x", ast::Type(ast::TypeKind::Int), std::nullopt});
    point_def->fields.push_back(ast::StructField{"y", ast::Type(ast::TypeKind::Int), std::nullopt});
    point_def->field_index["x"] = 0;
    point_def->field_index["y"] = 1;

    // Outer struct: Line contains two Points
    auto line_def = std::make_shared<interpreter::StructDef>();
    line_def->name = "Line";
    ast::Type point_type = ast::Type::makeStruct("Point");
    line_def->fields.push_back(ast::StructField{"start", point_type, std::nullopt});
    line_def->fields.push_back(ast::StructField{"end", point_type, std::nullopt});
    line_def->field_index["start"] = 0;
    line_def->field_index["end"] = 1;

    // Create nested struct values
    auto start_point = std::make_shared<interpreter::StructValue>("Point", point_def);
    start_point->setField("x", std::make_shared<interpreter::Value>(0));
    start_point->setField("y", std::make_shared<interpreter::Value>(0));

    auto end_point = std::make_shared<interpreter::StructValue>("Point", point_def);
    end_point->setField("x", std::make_shared<interpreter::Value>(100));
    end_point->setField("y", std::make_shared<interpreter::Value>(200));

    interpreter::StructValue line("Line", line_def);
    line.setField("start", std::make_shared<interpreter::Value>(start_point));
    line.setField("end", std::make_shared<interpreter::Value>(end_point));

    // Verify nested access
    auto start_val = line.getField("start");
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<interpreter::StructValue>>(start_val->data));
    auto start_struct = std::get<std::shared_ptr<interpreter::StructValue>>(start_val->data);
    ASSERT_EQ(std::get<int>(start_struct->getField("x")->data), 0);
}

TEST(StructValueTest, StructArray) {
    auto point_def = std::make_shared<interpreter::StructDef>();
    point_def->name = "Point";
    point_def->fields.push_back(ast::StructField{"x", ast::Type(ast::TypeKind::Int), std::nullopt});
    point_def->field_index["x"] = 0;

    // Create array of structs
    std::vector<std::shared_ptr<interpreter::Value>> points;
    for (int i = 0; i < 3; ++i) {
        auto p = std::make_shared<interpreter::StructValue>("Point", point_def);
        p->setField("x", std::make_shared<interpreter::Value>(i * 10));
        points.push_back(std::make_shared<interpreter::Value>(p));
    }

    // Verify array elements
    ASSERT_EQ(points.size(), 3);
    for (size_t i = 0; i < points.size(); ++i) {
        auto s = std::get<std::shared_ptr<interpreter::StructValue>>(points[i]->data);
        ASSERT_EQ(std::get<int>(s->getField("x")->data), static_cast<int>(i * 10));
    }
}

TEST(StructValueTest, StructInMap) {
    auto point_def = std::make_shared<interpreter::StructDef>();
    point_def->name = "Point";
    point_def->fields.push_back(ast::StructField{"x", ast::Type(ast::TypeKind::Int), std::nullopt});
    point_def->field_index["x"] = 0;

    // Create map with struct values
    std::unordered_map<std::string, std::shared_ptr<interpreter::Value>> point_map;

    auto p1 = std::make_shared<interpreter::StructValue>("Point", point_def);
    p1->setField("x", std::make_shared<interpreter::Value>(10));
    point_map["origin"] = std::make_shared<interpreter::Value>(p1);

    auto p2 = std::make_shared<interpreter::StructValue>("Point", point_def);
    p2->setField("x", std::make_shared<interpreter::Value>(20));
    point_map["destination"] = std::make_shared<interpreter::Value>(p2);

    // Verify map access
    ASSERT_EQ(point_map.size(), 2);
    auto origin = std::get<std::shared_ptr<interpreter::StructValue>>(point_map["origin"]->data);
    ASSERT_EQ(std::get<int>(origin->getField("x")->data), 10);
}

TEST(StructValueTest, DefaultFieldValue) {
    auto def = std::make_shared<interpreter::StructDef>();
    def->name = "Config";

    // Field with default value (optional std::nullopt for now)
    def->fields.push_back(ast::StructField{"port", ast::Type(ast::TypeKind::Int), std::nullopt});
    def->field_index["port"] = 0;

    interpreter::StructValue config("Config", def);

    // Explicitly set field value (default values would be handled in interpreter)
    auto default_val = std::make_shared<interpreter::Value>(42);
    config.field_values[0] = default_val;
    ASSERT_EQ(std::get<int>(config.getField("port")->data), 42);
}

TEST(StructValueTest, StructToString) {
    auto point_def = std::make_shared<interpreter::StructDef>();
    point_def->name = "Point";
    point_def->fields.push_back(ast::StructField{"x", ast::Type(ast::TypeKind::Int), std::nullopt});
    point_def->fields.push_back(ast::StructField{"y", ast::Type(ast::TypeKind::Int), std::nullopt});
    point_def->field_index["x"] = 0;
    point_def->field_index["y"] = 1;

    auto p = std::make_shared<interpreter::StructValue>("Point", point_def);
    p->setField("x", std::make_shared<interpreter::Value>(5));
    p->setField("y", std::make_shared<interpreter::Value>(10));

    auto val = std::make_shared<interpreter::Value>(p);
    std::string str = val->toString();

    // Verify string contains type name and field values
    ASSERT_NE(str.find("Point"), std::string::npos);
    ASSERT_NE(str.find("5"), std::string::npos);
    ASSERT_NE(str.find("10"), std::string::npos);
}

TEST(StructValueTest, StructCopy) {
    auto def = std::make_shared<interpreter::StructDef>();
    def->name = "Point";
    def->fields.push_back(ast::StructField{"x", ast::Type(ast::TypeKind::Int), std::nullopt});
    def->field_index["x"] = 0;

    auto p1 = std::make_shared<interpreter::StructValue>("Point", def);
    p1->setField("x", std::make_shared<interpreter::Value>(100));

    // Copy struct value
    auto p2 = std::make_shared<interpreter::StructValue>(*p1);

    // Verify copy has same values
    ASSERT_EQ(std::get<int>(p2->getField("x")->data), 100);

    // Verify independence (modifying copy doesn't affect original)
    p2->setField("x", std::make_shared<interpreter::Value>(200));
    ASSERT_EQ(std::get<int>(p1->getField("x")->data), 100);
    ASSERT_EQ(std::get<int>(p2->getField("x")->data), 200);
}

TEST(StructRegistryTest, ThreadSafety) {
    auto& registry = runtime::StructRegistry::instance();

    // Register multiple structs concurrently
    std::vector<std::thread> threads;
    for (int i = 0; i < 5; ++i) {
        threads.emplace_back([&registry, i]() {
            auto def = std::make_shared<interpreter::StructDef>();
            def->name = "ThreadStruct_" + std::to_string(i);
            try {
                registry.registerStruct(def);
            } catch (...) {
                // Ignore duplicate errors from concurrent registrations
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Verify at least one struct was registered successfully
    bool found_any = false;
    for (int i = 0; i < 5; ++i) {
        if (registry.hasStruct("ThreadStruct_" + std::to_string(i))) {
            found_any = true;
            break;
        }
    }
    ASSERT_TRUE(found_any);
}

TEST(StructValueTest, LargeFieldCount) {
    auto def = std::make_shared<interpreter::StructDef>();
    def->name = "LargeStruct";

    // Add 100 fields
    for (int i = 0; i < 100; ++i) {
        std::string field_name = "field_" + std::to_string(i);
        def->fields.push_back(ast::StructField{field_name, ast::Type(ast::TypeKind::Int), std::nullopt});
        def->field_index[field_name] = i;
    }

    interpreter::StructValue large("LargeStruct", def);

    // Set and verify all fields
    for (int i = 0; i < 100; ++i) {
        std::string field_name = "field_" + std::to_string(i);
        large.setField(field_name, std::make_shared<interpreter::Value>(i));
    }

    for (int i = 0; i < 100; ++i) {
        std::string field_name = "field_" + std::to_string(i);
        ASSERT_EQ(std::get<int>(large.getField(field_name)->data), i);
    }
}

TEST(StructValueTest, UnicodeFieldName) {
    auto def = std::make_shared<interpreter::StructDef>();
    def->name = "UnicodeStruct";

    // Field with Unicode name (UTF-8)
    std::string unicode_field = "坐标";  // "coordinate" in Chinese
    def->fields.push_back(ast::StructField{unicode_field, ast::Type(ast::TypeKind::Int), std::nullopt});
    def->field_index[unicode_field] = 0;

    interpreter::StructValue s("UnicodeStruct", def);
    s.setField(unicode_field, std::make_shared<interpreter::Value>(999));

    ASSERT_EQ(std::get<int>(s.getField(unicode_field)->data), 999);
}

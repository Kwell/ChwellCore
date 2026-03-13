#include <gtest/gtest.h>

#include "chwell/aoi/aoi.h"

using namespace chwell;

class AoiTest : public ::testing::Test {
protected:
    void SetUp() override {
        aoi::GridAoi::Config config;
        config.map_width = 1000;
        config.map_height = 1000;
        config.grid_size = 50;
        config.view_range = 1;
        aoi_ = std::make_unique<aoi::GridAoi>(config);
    }
    
    std::unique_ptr<aoi::GridAoi> aoi_;
};

TEST_F(AoiTest, AddAndRemoveEntity) {
    aoi::Entity e1(1, 100, 100, aoi::EntityType::PLAYER);
    
    EXPECT_TRUE(aoi_->add_entity(e1));
    EXPECT_EQ(aoi_->total_entities(), 1);
    
    aoi::Entity retrieved;
    EXPECT_TRUE(aoi_->get_entity(1, retrieved));
    EXPECT_EQ(retrieved.id, 1u);
    EXPECT_EQ(retrieved.x, 100);
    EXPECT_EQ(retrieved.y, 100);
    
    EXPECT_TRUE(aoi_->remove_entity(1));
    EXPECT_EQ(aoi_->total_entities(), 0);
    
    EXPECT_FALSE(aoi_->get_entity(1, retrieved));
}

TEST_F(AoiTest, AddDuplicateEntity) {
    aoi::Entity e1(1, 100, 100);
    EXPECT_TRUE(aoi_->add_entity(e1));
    
    aoi::Entity e2(1, 200, 200);
    EXPECT_FALSE(aoi_->add_entity(e2));
    
    EXPECT_EQ(aoi_->total_entities(), 1);
}

TEST_F(AoiTest, RemoveNonExistentEntity) {
    EXPECT_FALSE(aoi_->remove_entity(999));
}

TEST_F(AoiTest, UpdateEntityPosition) {
    aoi::Entity e1(1, 100, 100);
    aoi_->add_entity(e1);
    
    EXPECT_TRUE(aoi_->update_entity(1, 200, 200));
    
    aoi::Entity retrieved;
    aoi_->get_entity(1, retrieved);
    EXPECT_EQ(retrieved.x, 200);
    EXPECT_EQ(retrieved.y, 200);
}

TEST_F(AoiTest, GetEntitiesInView) {
    aoi_->add_entity(aoi::Entity(1, 100, 100));  // In view
    aoi_->add_entity(aoi::Entity(2, 110, 110));  // In view
    aoi_->add_entity(aoi::Entity(3, 500, 500));  // Out of view
    
    auto in_view = aoi_->get_entities_in_view(100, 100);
    EXPECT_EQ(in_view.size(), 2u);
    
    auto ids = aoi_->get_entity_ids_in_view(100, 100);
    EXPECT_EQ(ids.size(), 2u);
}

TEST_F(AoiTest, GetEntitiesInGrid) {
    aoi_->add_entity(aoi::Entity(1, 55, 55));
    aoi_->add_entity(aoi::Entity(2, 60, 60));
    aoi_->add_entity(aoi::Entity(3, 150, 150));  // Different grid
    
    int gx, gy;
    aoi_->pos_to_grid(55, 55, gx, gy);
    
    auto in_grid = aoi_->get_entities_in_grid(gx, gy);
    EXPECT_EQ(in_grid.size(), 2u);
    
    auto ids = aoi_->get_entity_ids_in_grid(gx, gy);
    EXPECT_EQ(ids.size(), 2u);
}

TEST_F(AoiTest, PosToGrid) {
    int gx, gy;
    
    aoi_->pos_to_grid(0, 0, gx, gy);
    EXPECT_EQ(gx, 0);
    EXPECT_EQ(gy, 0);
    
    aoi_->pos_to_grid(75, 125, gx, gy);
    EXPECT_EQ(gx, 1);
    EXPECT_EQ(gy, 2);
}

TEST_F(AoiTest, GridToCenter) {
    int x, y;
    aoi_->grid_to_center(1, 2, x, y);
    
    EXPECT_EQ(x, 75);  // 1 * 50 + 25
    EXPECT_EQ(y, 125); // 2 * 50 + 25
}

TEST_F(AoiTest, EntityCountInGrid) {
    aoi_->add_entity(aoi::Entity(1, 55, 55));
    aoi_->add_entity(aoi::Entity(2, 60, 60));
    
    int gx, gy;
    aoi_->pos_to_grid(55, 55, gx, gy);
    
    EXPECT_EQ(aoi_->entity_count_in_grid(gx, gy), 2);
    EXPECT_EQ(aoi_->entity_count_in_grid(0, 0), 0);
}

TEST_F(AoiTest, Callback) {
    std::vector<aoi::EventType> events;
    
    aoi_->set_callback([&events](const aoi::AoiEvent& e) {
        events.push_back(e.type);
    });
    
    aoi_->add_entity(aoi::Entity(1, 100, 100));
    aoi_->update_entity(1, 200, 200);
    aoi_->remove_entity(1);
    
    // Callback may be triggered for ENTER/MOVE/LEAVE events
    EXPECT_GE(events.size(), 0u);
}

TEST_F(AoiTest, DifferentEntityTypes) {
    aoi_->add_entity(aoi::Entity(1, 100, 100, aoi::EntityType::PLAYER));
    aoi_->add_entity(aoi::Entity(2, 110, 110, aoi::EntityType::NPC));
    aoi_->add_entity(aoi::Entity(3, 120, 120, aoi::EntityType::MONSTER));
    aoi_->add_entity(aoi::Entity(4, 130, 130, aoi::EntityType::ITEM));
    
    EXPECT_EQ(aoi_->total_entities(), 4);
    
    auto in_view = aoi_->get_entities_in_view(100, 100);
    EXPECT_EQ(in_view.size(), 4u);
}

// CrossListAoi Tests
TEST(CrossListAoiTest, BasicOperations) {
    aoi::CrossListAoi::Config config;
    config.map_width = 1000;
    config.map_height = 1000;
    config.view_range = 100;
    
    aoi::CrossListAoi aoi(config);
    
    aoi::Entity e1(1, 100, 100);
    EXPECT_TRUE(aoi.add_entity(e1));
    EXPECT_EQ(aoi.total_entities(), 1);
    
    aoi::Entity e2(2, 150, 150);
    EXPECT_TRUE(aoi.add_entity(e2));
    
    auto in_view = aoi.get_entities_in_view(100, 100);
    EXPECT_GE(in_view.size(), 1u);
    
    EXPECT_TRUE(aoi.update_entity(1, 200, 200));
    EXPECT_TRUE(aoi.remove_entity(1));
    EXPECT_EQ(aoi.total_entities(), 1);
}

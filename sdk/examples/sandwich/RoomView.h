#pragma once
#include "Common.h"
#include "View.h"
#include "Content.h"

class Room;
#define ANIM_TILE_CAPACITY 4

class RoomView : public View {
private:
  uint8_t mRoomId;

  uint8_t mStartFrame;
  uint8_t mAnimTileCount;
  struct {
    uint8_t hideOverlay : 1;
  } flags;

  struct AnimTile {
    uint8_t lid;
    uint8_t frameCount;
  };
  AnimTile mAnimTiles[ANIM_TILE_CAPACITY];


public:  
  // getters
  Vec2 Location() const;
  Room* GetRoom() const;
  
  // methods
  void Init(unsigned rid);
  void Restore();
  void Update();

  void HideOverlay(bool flag);
  
  void ShowPlayer();
  void SetPlayerFrame(unsigned frame);
  void UpdatePlayer();
  void HidePlayer();
  
  void SetItemPosition(Vec2 p);
  void HideItem();

  void DrawBackground();

private:
  void ComputeAnimatedTiles();

  // misc hacky stuff
  //void RandomizeBff();
};
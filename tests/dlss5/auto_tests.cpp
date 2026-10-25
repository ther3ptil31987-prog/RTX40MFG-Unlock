#include "dlss5_auto.h"
#include <cstdio>
#include <cstdlib>
unsigned checks=0;
#define CHECK(c) do{++checks;if(!(c)){std::fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#c);std::exit(1);}}while(0)
using namespace dlss5;
using namespace dlss5::auto_mode;
// Stage cost model: proportional to processed pixels.
float Cost(float fullMs,float scale){return fullMs*(scale/100.0f)*(scale/100.0f);}
// Runs the controller at 60 rendered frames per second; returns the number of level changes.
unsigned Run(ScaleController& controller,float fullMs,float budget,double seconds,size_t ceiling){
 unsigned steps=0;
 for(double t=0;t<seconds;t+=1.0/60.0)steps+=controller.Update(Cost(fullMs,controller.Scale()),budget,1.0/60.0,ceiling);
 return steps;
}
// autoscale.1: a GPU-bound game whose frame is `rest` ms without DLSS 5, and a
// stage with a fixed part (the FF7 Rebirth log, RTX 3080 Ti at 1080p: 7.06 ms
// at 100 %, 3.91 ms at 50 %). The budget comes from the measured frame and stage
// as Harvest computes it. Returns the number of level changes.
float StageMs(float scale){return 2.86f+4.20f*(scale/100.0f)*(scale/100.0f);}
unsigned RunFrame(ScaleController& controller,float rest,double seconds,size_t ceiling,float* budget=nullptr){
 unsigned steps=0;
 for(double t=0;t<seconds;t+=1.0/60.0){
  const float stage=StageMs(controller.Scale()),limit=BudgetMs(rest+stage,stage);
  if(budget)*budget=limit;
  steps+=controller.Update(stage,limit,1.0/60.0,ceiling);
 }
 return steps;
}
int main(){
 CHECK(AutoStage()==Stage::eUpscaled);
 CHECK(kLevels[StartingLevel(1920,1080)]==100.0f);CHECK(kLevels[StartingLevel(2560,1440)]==100.0f);
 CHECK(kLevels[StartingLevel(3840,2160)]==75.0f);CHECK(kLevels[StartingLevel(7680,4320)]==75.0f);
 CHECK(CeilingLevel(200)==0);CHECK(CeilingLevel(100)==0);CHECK(kLevels[CeilingLevel(80)]==75.0f);
 CHECK(kLevels[CeilingLevel(66)]==66.0f);CHECK(kLevels[CeilingLevel(10)]==33.0f);
 CHECK(BudgetMs(0)==kMaxBudgetMs);CHECK(std::abs(BudgetMs(1000.0f/60.0f)-5.0f)<1e-4f);CHECK(BudgetMs(1.0f)==kMinBudgetMs);CHECK(BudgetMs(100.0f)==kMaxBudgetMs);
 // The quality floor is 75 %; a slider below it is the floor itself.
 CHECK(kLevels[kFloorLevel]==75.0f&&FloorLevel(0)==kFloorLevel&&FloorLevel(CeilingLevel(85))==kFloorLevel);
 CHECK(FloorLevel(CeilingLevel(50))==CeilingLevel(50)&&FloorLevel(99)==kLevels.size()-1);
 // The budget comes from the frame without the stage: the same at every level of
 // the FF7 Rebirth case (the old 30 % of the whole interval fell 3.80 -> 2.86 ms).
 for(const float scale:kLevels){
  const float budget=BudgetMs(5.61f+StageMs(scale),StageMs(scale));
  CHECK(std::abs(budget-5.61f*kBudgetShare/(1.0f-kBudgetShare))<1e-3f);
 }
 CHECK(BudgetMs(10.0f,12.0f)==kMinBudgetMs&&BudgetMs(10.0f,std::nanf(""))==BudgetMs(10.0f));
 // Overload: no change during the minimum interval, then one step per interval.
 ScaleController c;c.Reset(0);
 CHECK(Run(c,10.0f,5.0f,7.9,0)==0&&c.level==0);
 CHECK(Run(c,10.0f,5.0f,0.2,0)==1&&c.level==1);
 CHECK(Run(c,10.0f,5.0f,7.0,0)==0&&c.level==1);
 // Converges inside the band (10 ms at native, 5 ms budget: 75 % = 5.6 ms is within
 // 15 % of the budget, 85 % would not fit) and stays.
 c.Reset(0);const unsigned steps=Run(c,10.0f,5.0f,120.0,0);
 CHECK(c.Scale()==75.0f&&steps==2);
 CHECK(Run(c,10.0f,5.0f,120.0,0)==0&&c.Scale()==75.0f);
 // A heavier stage stops at the floor (14 ms native: 75 % = 7.9 ms is still over the band)
 // and says so; the frame rate pays, not the detail.
 c.Reset(0);CHECK(Run(c,14.0f,5.0f,120.0,0)==2&&c.Scale()==75.0f&&c.floorHeld);
 // More headroom: back up, one level per interval, not above the ceiling.
 CHECK(Run(c,4.0f,5.0f,60.0,0)>=1&&c.Scale()==100.0f);
 c.Reset(0);CHECK(Run(c,1.0f,5.0f,30.0,CeilingLevel(75))>=1&&c.Scale()==75.0f);
 CHECK(Run(c,1.0f,5.0f,60.0,CeilingLevel(75))==0&&c.Scale()==75.0f);
 // Lowering the slider takes effect at once.
 c.Reset(0);CHECK(c.Update(1.0f,5.0f,0.016,CeilingLevel(50))&&c.Scale()==50.0f);
 // A slider below the floor: Auto holds the slider's level, never below it, and the
 // floor note stays off (the user chose that scale).
 c.Reset(0);CHECK(Run(c,100.0f,2.0f,60.0,CeilingLevel(50))==1&&c.Scale()==50.0f&&!c.floorHeld);
 // A level below the floor (an older controller state) comes back up to it.
 c.Reset(kLevels.size()-1);CHECK(c.Clamp(0)&&c.Scale()==75.0f);
 c.Reset(kLevels.size()-1);CHECK(Run(c,100.0f,2.0f,60.0,0)==1&&c.Scale()==75.0f&&c.floorHeld);
 // Invalid measurements ignored.
 c.Reset(0);CHECK(!c.Update(-1.0f,5.0f,0.016,0)&&!c.Update(1.0f,5.0f,-1.0,0)&&c.samples==0);
 // The FF7 Rebirth case: from 100 % to the floor in two steps and held there
 // (was 100 -> 33 % in five), with a budget that stays put.
 {
  ScaleController ff7;ff7.Reset(0);float budget=0.0f;
  CHECK(RunFrame(ff7,5.61f,120.0,0,&budget)==2&&ff7.Scale()==75.0f&&ff7.floorHeld);
  CHECK(std::abs(budget-2.40f)<0.01f);
  CHECK(RunFrame(ff7,5.61f,120.0,0)==0&&ff7.Scale()==75.0f&&ff7.floorHeld);
  // A heavier scene gives a larger budget: up again (85 % fits, 100 % would not), floor note off.
  CHECK(RunFrame(ff7,20.0f,60.0,0,&budget)==1&&ff7.Scale()==85.0f&&!ff7.floorHeld&&budget>8.0f);
  CHECK(RunFrame(ff7,20.0f,60.0,0)==0&&ff7.Scale()==85.0f);
  // Plenty of headroom: back to the slider's 100 %, never above it.
  CHECK(RunFrame(ff7,40.0f,60.0,0)==1&&ff7.Scale()==100.0f);
 }
 // Keeping: the live feature's creation settings, everything else from the new ones.
 {
  Settings applied{};applied.scale=100.0f;applied.model=0;applied.passes=1;applied.preset=0;
  Settings next=applied;next.scale=75.0f;next.model=1;next.passes=2;next.preset=2;next.intensity=0.3f;next.structure=0.9f;
  const Settings live=Keeping(next,applied);
  CHECK(SameFeature(live,applied)&&live.intensity==0.3f&&live.structure==0.9f&&!SameFeature(next,applied));
 }
 // Exposure: first measurement sets the gain, later ones smooth toward middle grey, clamped.
 ExposureController e;e.Update(std::log2(0.18f),0.016);CHECK(e.valid&&std::abs(e.Gain()-1.0f)<1e-5f);
 e.Reset();e.Update(std::log2(0.045f),0.016);CHECK(std::abs(e.Gain()-4.0f)<1e-4f);
 e.Update(std::log2(0.18f),0.1);CHECK(e.Gain()<4.0f&&e.Gain()>1.0f);
 for(int n=0;n<200;++n)e.Update(std::log2(0.18f),0.05);CHECK(std::abs(e.Gain()-1.0f)<1e-3f);
 e.Reset();e.Update(std::log2(1e-6f),0.016);CHECK(e.Gain()==ExposureController::kMaxGain);
 e.Reset();e.Update(std::log2(1e6f),0.016);CHECK(e.Gain()==ExposureController::kMinGain);
 e.Reset();e.Update(std::nanf(""),0.016);CHECK(!e.valid&&e.Gain()==1.0f);
 // Adaptive target (dlss5adv.1): the luminance the gain aims for; an invalid one falls back to middle grey.
 e.Reset();e.Update(std::log2(0.09f),0.016,0.09f);CHECK(std::abs(e.Gain()-1.0f)<1e-5f);
 e.Reset();e.Update(std::log2(0.09f),0.016,0.36f);CHECK(std::abs(e.Gain()-4.0f)<1e-4f);
 e.Reset();e.Update(std::log2(0.18f),0.016,-1.0f);CHECK(std::abs(e.Gain()-1.0f)<1e-5f);
 // Frame clock: steady intervals, pauses ignored.
 FrameClock clock;CHECK(clock.Tick(1000,1000)==0.0);CHECK(std::abs(clock.Tick(1016,1000)-0.016)<1e-9);
 CHECK(clock.averageMs==16.0f);CHECK(clock.Tick(2016,1000)==0.0&&clock.averageMs==16.0f);
 CHECK(clock.Tick(2032,1000)>0.0);CHECK(clock.Tick(2032,0)==0.0);
 // Looks (dlss5.8d): the default settings are Balanced; each look matches
 // itself after ApplyLook and leaves Pass Count, Resolution Scale and the
 // compare settings alone; anything else is Custom.
 Settings look{};CHECK(MatchingLook(look)==1);
 look.passes=3;look.scale=50.0f;look.compare=true;look.compareKey=0x70;
 for(size_t n=0;n<kLookCount;++n){ApplyLook(look,n);CHECK(MatchingLook(look)==n);CHECK(look.passes==3&&look.scale==50.0f&&look.compare&&look.compareKey==0x70);}
 // autoscale.2: Balanced is the provider's own default (Intensity, Structure, Tone 1, Faces & skin following
 // Structure), Subtle half its intensity, Strong further than Balanced.
 CHECK(kLooks[1].intensity==1.0f&&kLooks[1].model==0&&kLooks[1].structure==1.0f&&kLooks[1].tone==1.0f&&kLooks[1].character==-1.0f);
 CHECK(kLooks[0].intensity==0.5f&&kLooks[0].structure==kLooks[1].structure&&kLooks[0].tone==kLooks[1].tone&&kLooks[0].character==-1.0f);
 CHECK(kLooks[2].intensity==1.0f&&kLooks[2].structure>kLooks[1].structure&&kLooks[2].tone>kLooks[1].tone&&kLooks[2].structure<=kMaxStrength);
 CHECK(!Settings{}.scaleAuto&&Settings{}.scale==100.0f);
 look.tone=0.45f;CHECK(MatchingLook(look)==kLookCount);ApplyLook(look,kLookCount);CHECK(look.tone==0.45f);
 // Compare settings are sanitized: split within 5-95 %, no mouse buttons, Escape or Windows keys.
 Settings compare{};CHECK(!compare.compare&&compare.compareSplit==50.0f&&compare.compareKey==0);
 compare.compareSplit=200.0f;compare.compareKey=0x1B;CHECK(Sanitized(compare).compareSplit==kMaxCompareSplit&&Sanitized(compare).compareKey==0);
 compare.compareSplit=1.0f;compare.compareKey=0x01;CHECK(Sanitized(compare).compareSplit==kMinCompareSplit&&Sanitized(compare).compareKey==0);
 compare.compareKey=0x5B;CHECK(Sanitized(compare).compareKey==0);compare.compareKey=0x06;CHECK(Sanitized(compare).compareKey==0x06);
 compare.compareKey=0x70;CHECK(Sanitized(compare).compareKey==0x70);compare.compareKey=0xFF;CHECK(Sanitized(compare).compareKey==0);
 std::printf("DLSS5 auto checks=%u PASS\n",checks);
}

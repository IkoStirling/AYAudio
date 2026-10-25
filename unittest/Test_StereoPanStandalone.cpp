#include <AYAudio/StereoPan.h>
#include <AYTestFixtures.h>
#include <limits>
TEST_SUITE(StereoPan)
TEST_CASE(direction_is_symmetric_and_power_is_bounded){
    for(int i=-100;i<=100;++i){const auto gains=ayt::audio::equalPowerStereoPan(i/100.0f),mirror=ayt::audio::equalPowerStereoPan(-i/100.0f);CHECK(gains.left>=0&&gains.left<=1);CHECK(gains.right>=0&&gains.right<=1);CHECK(std::abs(gains.left*gains.left+gains.right*gains.right-1)<0.00001f);CHECK(std::abs(gains.left-mirror.right)<0.00001f);if(i<0)CHECK(gains.left>gains.right);if(i>0)CHECK(gains.right>gains.left);}
    const auto left=ayt::audio::equalPowerStereoPan(-2),right=ayt::audio::equalPowerStereoPan(2);CHECK(left.left==1&&left.right==0);CHECK(right.right==1&&right.left<0.00001f);
}
TEST_CASE(non_finite_positions_cannot_poison_output){
    const auto center=ayt::audio::equalPowerStereoPan(0);for(const float value:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}){const auto gains=ayt::audio::equalPowerStereoPan(value);CHECK(gains.left==center.left);CHECK(gains.right==center.right);}
}
TEST_SUITE_END
int main(int argc,char** argv){return ayt::test::runTests("AYAudioPan",argc,argv);}

/** 
 * @file fsperfstats.cpp
 * @brief Stats collection to support perf floater and auto tune
 *
 * $LicenseInfo:firstyear=2021&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2021, The Phoenix Firestorm Project, Inc.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * 
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include "fsperfstats.h"
#include "llcontrol.h"
#include "pipeline.h"
#include "llagentcamera.h"
#include "llvoavatar.h"
#include "llworld.h"
#include <llthread.h>

extern LLControlGroup gSavedSettings;

namespace FSPerfStats
{
 #ifdef USAGE_TRACKING
    std::atomic<int64_t> inUse{0};
    std::atomic<int64_t> inUseAvatar{0};
    std::atomic<int64_t> inUseScene{0};
    std::atomic<int64_t> inUseAttachment{0};
    std::atomic<int64_t> inUseAttachmentRigged{0};
    std::atomic<int64_t> inUseAttachmentUnRigged{0};
#endif
    
    std::atomic<int64_t> tunedAvatars{0};
    std::atomic<U64> renderAvatarMaxART_ns{(U64)(ART_UNLIMITED_NANOS)}; // highest render time we'll allow without culling features
    bool belowTargetFPS{false};
    U32 lastGlobalPrefChange{0}; 
    std::mutex bufferToggleLock{};

    Tunables tunables;

    std::atomic<int> 	StatsRecorder::writeBuffer{0};
    bool 	            StatsRecorder::collectionEnabled{true};
    LLUUID              StatsRecorder::focusAv{LLUUID::null};
	std::array<StatsRecorder::StatsTypeMatrix,2>  StatsRecorder::statsDoubleBuffer{ {} };
    std::array<StatsRecorder::StatsSummaryArray,2> StatsRecorder::max{ {} };
    std::array<StatsRecorder::StatsSummaryArray,2> StatsRecorder::sum{ {} };

    void Tunables::applyUpdates()
    {
        assert_main_thread();
        // these following variables are proxies for pipeline statics we do not need a two way update (no llviewercontrol handler)
        if( tuningFlag & NonImpostors ){ gSavedSettings.setU32("IndirectMaxNonImpostors", nonImpostors); };
        if( tuningFlag & ReflectionDetail ){ gSavedSettings.setS32("RenderReflectionDetail", reflectionDetail); };
        if( tuningFlag & FarClip ){ gSavedSettings.setF32("RenderFarClip", farClip); };
        if( tuningFlag & UserMinDrawDistance ){ gSavedSettings.setF32("FSAutoTuneRenderFarClipMin", userMinDrawDistance); };
        if( tuningFlag & UserTargetDrawDistance ){ gSavedSettings.setF32("FSAutoTuneRenderFarClipTarget", userTargetDrawDistance); };
        if( tuningFlag & UserImpostorDistance ){ gSavedSettings.setF32("FSAutoTuneImpostorFarAwayDistance", userImpostorDistance); };
        if( tuningFlag & UserImpostorDistanceTuningEnabled ){ gSavedSettings.setBOOL("FSAutoTuneImpostorByDistEnabled", userImpostorDistanceTuningEnabled); };
        if( tuningFlag & UserFPSTuningStrategy ){ gSavedSettings.setU32("FSTuningFPSStrategy", userFPSTuningStrategy); };
        if( tuningFlag & UserAutoTuneEnabled ){ gSavedSettings.setBOOL("FSAutoTuneFPS", userAutoTuneEnabled); };
        if( tuningFlag & UserAutoTuneLock ){ gSavedSettings.setBOOL("FSAutoTuneLock", userAutoTuneLock); };
        if( tuningFlag & UserTargetFPS ){ gSavedSettings.setU32("FSTargetFPS", userTargetFPS); };
        if( tuningFlag & UserTargetReflections ){ gSavedSettings.setS32("FSUserTargetReflections", userTargetReflections); };
        // Note: The Max ART slider is logarithmic and thus we have an intermediate proxy value
        if( tuningFlag & UserARTCutoff ){ gSavedSettings.setF32("FSRenderAvatarMaxART", userARTCutoffSliderValue); };
        resetChanges();
    }

    void Tunables::updateRenderCostLimitFromSettings()
    {
        assert_main_thread();
	    const auto newval = gSavedSettings.getF32("FSRenderAvatarMaxART");
	    if(newval < log10(FSPerfStats::ART_UNLIMITED_NANOS/1000))
	    {
    		FSPerfStats::renderAvatarMaxART_ns = pow(10,newval)*1000;
    	}
    	else
    	{
		    FSPerfStats::renderAvatarMaxART_ns = 0;
	    };        
    }

    // static 
    void Tunables::updateSettingsFromRenderCostLimit()
    {
        if( userARTCutoffSliderValue != log10( ( (F32)FSPerfStats::renderAvatarMaxART_ns )/1000 ) )
        {
            if( FSPerfStats::renderAvatarMaxART_ns != 0 )
            {
                updateUserARTCutoffSlider(log10( ( (F32)FSPerfStats::renderAvatarMaxART_ns )/1000 ) );
            }
            else
            {
                updateUserARTCutoffSlider(log10( (F32)FSPerfStats::ART_UNLIMITED_NANOS/1000 ) );
            }
        }        
    }

    void Tunables::initialiseFromSettings()
    {
        assert_main_thread();
        // the following variables are two way and have "push" in llviewercontrol 
        FSPerfStats::tunables.userMinDrawDistance = gSavedSettings.getF32("FSAutoTuneRenderFarClipMin");
        FSPerfStats::tunables.userTargetDrawDistance = gSavedSettings.getF32("FSAutoTuneRenderFarClipTarget");
        FSPerfStats::tunables.userImpostorDistance = gSavedSettings.getF32("FSAutoTuneImpostorFarAwayDistance");
        FSPerfStats::tunables.userImpostorDistanceTuningEnabled = gSavedSettings.getBOOL("FSAutoTuneImpostorByDistEnabled");
        FSPerfStats::tunables.userFPSTuningStrategy = gSavedSettings.getU32("FSTuningFPSStrategy");
        FSPerfStats::tunables.userTargetFPS = gSavedSettings.getU32("FSTargetFPS");
        FSPerfStats::tunables.userTargetReflections = gSavedSettings.getU32("FSUserTargetReflections");
        FSPerfStats::tunables.userAutoTuneEnabled = gSavedSettings.getBOOL("FSAutoTuneFPS");
        FSPerfStats::tunables.userAutoTuneLock = gSavedSettings.getBOOL("FSAutoTuneLock");
        // Note: The Max ART slider is logarithmic and thus we have an intermediate proxy value
        updateRenderCostLimitFromSettings();
        resetChanges();
    }

    StatsRecorder::StatsRecorder():q(1024*16),t(&StatsRecorder::run)
    {
        // create a queue
        // create a thread to consume from the queue
        tunables.initialiseFromSettings();
        t.detach();
    }

    // static
    void StatsRecorder::toggleBuffer()
    {
        FSZone;
        using ST = StatType_t;

        bool unreliable{false};
        FSPerfStats::StatsRecorder::getSceneStat(FSPerfStats::StatType_t::RENDER_FRAME);
        auto& sceneStats = statsDoubleBuffer[writeBuffer][static_cast<size_t>(ObjType_t::OT_GENERAL)][LLUUID::null];
        auto& lastStats = statsDoubleBuffer[writeBuffer ^ 1][static_cast<size_t>(ObjType_t::OT_GENERAL)][LLUUID::null];

        static constexpr std::initializer_list<StatType_t> sceneStatsToAvg = {
            StatType_t::RENDER_FRAME, 
            StatType_t::RENDER_DISPLAY, 
            StatType_t::RENDER_HUDS,
            StatType_t::RENDER_UI,
            StatType_t::RENDER_SWAP,
            // RENDER_LFS,
            // RENDER_MESHREPO,
            StatType_t::RENDER_IDLE };

        static constexpr std::initializer_list<StatType_t> avatarStatsToAvg = {
            StatType_t::RENDER_GEOMETRY, 
            StatType_t::RENDER_SHADOWS, 
            StatType_t::RENDER_COMBINED,
            StatType_t::RENDER_IDLE };


        if( sceneStats[static_cast<size_t>(StatType_t::RENDER_FPSLIMIT)] != 0 || sceneStats[static_cast<size_t>(StatType_t::RENDER_SLEEP)] != 0 )
        {
            unreliable = true;
            lastStats[static_cast<size_t>(StatType_t::RENDER_FPSLIMIT)] = sceneStats[static_cast<size_t>(StatType_t::RENDER_FPSLIMIT)];
            lastStats[static_cast<size_t>(StatType_t::RENDER_SLEEP)] = sceneStats[static_cast<size_t>(StatType_t::RENDER_SLEEP)];
            lastStats[static_cast<size_t>(StatType_t::RENDER_FRAME)] = sceneStats[static_cast<size_t>(StatType_t::RENDER_FRAME)]; //  bring over the total frame render time to deal with region crossing overlap issues
        }

        if(!unreliable)
        {
            // only use these stats when things are reliable. 

            for(auto & statEntry : sceneStatsToAvg)
            {
                auto avg = lastStats[static_cast<size_t>(statEntry)];
                auto val = sceneStats[static_cast<size_t>(statEntry)];
                sceneStats[static_cast<size_t>(statEntry)] = avg + (val / SMOOTHING_PERIODS) - (avg / SMOOTHING_PERIODS);
                // LL_INFOS("scenestats") << "Scenestat: " << static_cast<size_t>(statEntry) << " before=" << avg << " new=" << val << " newavg=" << statsDoubleBuffer[writeBuffer][static_cast<size_t>(ObjType_t::OT_GENERAL)][LLUUID::null][static_cast<size_t>(statEntry)] << LL_ENDL;
            }

            auto& statsMap = statsDoubleBuffer[writeBuffer][static_cast<size_t>(ObjType_t::OT_ATTACHMENT)];
            for(auto& stat_entry : statsMap)
            {
                auto val = stat_entry.second[static_cast<size_t>(ST::RENDER_COMBINED)];
                if(val > SMOOTHING_PERIODS){
                    auto avg = statsDoubleBuffer[writeBuffer ^ 1][static_cast<size_t>(ObjType_t::OT_ATTACHMENT)][stat_entry.first][static_cast<size_t>(ST::RENDER_COMBINED)];
                    stat_entry.second[static_cast<size_t>(ST::RENDER_COMBINED)] = avg + (val / SMOOTHING_PERIODS) - (avg / SMOOTHING_PERIODS);
                }
            }


            auto& statsMapAv = statsDoubleBuffer[writeBuffer][static_cast<size_t>(ObjType_t::OT_AVATAR)];
            for(auto& stat_entry : statsMapAv)
            {
                for(auto& stat : avatarStatsToAvg)
                {
                    auto val = stat_entry.second[static_cast<size_t>(stat)];
                    if(val > SMOOTHING_PERIODS)
                    {
                        auto avg = statsDoubleBuffer[writeBuffer ^ 1][static_cast<size_t>(ObjType_t::OT_AVATAR)][stat_entry.first][static_cast<size_t>(stat)];
                        stat_entry.second[static_cast<size_t>(stat)] = avg + (val / SMOOTHING_PERIODS) - (avg / SMOOTHING_PERIODS);
                    }
                }
            }

            // swap the buffers
            if(enabled())
            {
                std::lock_guard<std::mutex> lock{bufferToggleLock};
                writeBuffer ^= 1;
            }; // note we are relying on atomic updates here. The risk is low and would cause minor errors in the stats display. 
        }

        // clean the write maps in all cases.
        auto& statsTypeMatrix = statsDoubleBuffer[writeBuffer];
        for(auto& statsMap : statsTypeMatrix)
        {
            FSZoneN("Clear stats maps");
            for(auto& stat_entry : statsMap)
            {
                std::fill_n(stat_entry.second.begin() ,static_cast<size_t>(ST::STATS_COUNT),0);
            }
            statsMap.clear();
        }
        for(int i=0; i< static_cast<size_t>(ObjType_t::OT_COUNT); i++)
        {
            FSZoneN("clear max/sum");
            max[writeBuffer][i].fill(0);
            sum[writeBuffer][i].fill(0);
        }

        // and now adjust the proxy vars so that the main thread can adjust the visuals.
        if(tunables.userAutoTuneEnabled)
        {
            updateAvatarParams();
        }
    }

    // clear buffers when we change region or need a hard reset.
    // static 
    void StatsRecorder::clearStatsBuffers()
    {
        FSZone;
        using ST = StatType_t;

        auto& statsTypeMatrix = statsDoubleBuffer[writeBuffer];
        for(auto& statsMap : statsTypeMatrix)
        {
            FSZoneN("Clear stats maps");
            for(auto& stat_entry : statsMap)
            {
                std::fill_n(stat_entry.second.begin() ,static_cast<size_t>(ST::STATS_COUNT),0);
            }
            statsMap.clear();
        }
        for(int i=0; i< static_cast<size_t>(ObjType_t::OT_COUNT); i++)
        {
            FSZoneN("clear max/sum");
            max[writeBuffer][i].fill(0);
            sum[writeBuffer][i].fill(0);
        }
        // swap the clean buffers in
        if(enabled())
        {
            std::lock_guard<std::mutex> lock{bufferToggleLock};
            writeBuffer ^= 1;
        }; 
        // repeat before we start processing new stuff
        for(auto& statsMap : statsTypeMatrix)
        {
            FSZoneN("Clear stats maps");
            for(auto& stat_entry : statsMap)
            {
                std::fill_n(stat_entry.second.begin() ,static_cast<size_t>(ST::STATS_COUNT),0);
            }
            statsMap.clear();
        }
        for(int i=0; i< static_cast<size_t>(ObjType_t::OT_COUNT); i++)
        {
            FSZoneN("clear max/sum");
            max[writeBuffer][i].fill(0);
            sum[writeBuffer][i].fill(0);
        }
    }

    //static
    int StatsRecorder::countNearbyAvatars(S32 distance)
    {
        const auto our_pos = gAgentCamera.getCameraPositionGlobal();

       	std::vector<LLVector3d> positions;
	    uuid_vec_t avatar_ids;
        LLWorld::getInstance()->getAvatars(&avatar_ids, &positions, our_pos, distance);
        return positions.size();
	}

    // static
    void StatsRecorder::updateAvatarParams()
    {

        if(tunables.userImpostorDistanceTuningEnabled)
        {
            // if we have less than the user's "max Non-Impostors" avatars within the desired range then adjust the limit.
            // also adjusts back up again for nearby crowds.
            auto count = countNearbyAvatars(std::min(LLPipeline::RenderFarClip, tunables.userImpostorDistance));
            if( count != tunables.nonImpostors )
            {
                tunables.updateNonImposters( (count < LLVOAvatar::NON_IMPOSTORS_MAX_SLIDER)?count : LLVOAvatar::NON_IMPOSTORS_MAX_SLIDER );
                LL_DEBUGS("AutoTune") << "There are " << count << "avatars within " << std::min(LLPipeline::RenderFarClip, tunables.userImpostorDistance) << "m of the camera" << LL_ENDL;
            }
        }

        auto av_render_max_raw = FSPerfStats::StatsRecorder::getMax(ObjType_t::OT_AVATAR, FSPerfStats::StatType_t::RENDER_COMBINED);
        // Is our target frame time lower than current? If so we need to take action to reduce draw overheads.
        // cumulative avatar time (includes idle processing, attachments and base av)
        auto tot_avatar_time_raw = FSPerfStats::StatsRecorder::getSum(ObjType_t::OT_AVATAR, FSPerfStats::StatType_t::RENDER_COMBINED);
        // sleep time is basically forced sleep when window out of focus 
        auto tot_sleep_time_raw = FSPerfStats::StatsRecorder::getSceneStat(FSPerfStats::StatType_t::RENDER_SLEEP);
        // similar to sleep time, induced by FPS limit
        auto tot_limit_time_raw = FSPerfStats::StatsRecorder::getSceneStat(FSPerfStats::StatType_t::RENDER_FPSLIMIT);


        // the time spent this frame on the "doFrame" call. Treated as "tot time for frame"
        auto tot_frame_time_raw = FSPerfStats::StatsRecorder::getSceneStat(FSPerfStats::StatType_t::RENDER_FRAME);

        if( tot_sleep_time_raw != 0 )
        {
            // Note: we do not average sleep 
            // if at some point we need to, the averaging will need to take this into account or 
            // we forever think we're in the background due to residuals.
            LL_DEBUGS() << "No tuning when not in focus" << LL_ENDL;
            return;
        }

        // The frametime budget we have based on the target FPS selected
        auto target_frame_time_raw = (U64)llround((F64)LLTrace::BlockTimer::countsPerSecond()/(tunables.userTargetFPS==0?1:tunables.userTargetFPS));
        // LL_INFOS() << "Effective FPS(raw):" << tot_frame_time_raw << " Target:" << target_frame_time_raw << LL_ENDL;
        auto inferredFPS{1000/(U32)std::max(raw_to_ms(tot_frame_time_raw),1.0)};
        U32 settingsChangeFrequency{inferredFPS > 25?inferredFPS:25};
        if( tot_limit_time_raw != 0)
        {
            // This could be problematic.
            tot_frame_time_raw -= tot_limit_time_raw;
        }
        // 1) Is the target frame time lower than current?
        if( target_frame_time_raw <= tot_frame_time_raw )
        {
            if(belowTargetFPS == false)
            {
                // this is the first frame under. hold fire to add a little hysteresis
                belowTargetFPS = true;
                FSPerfStats::lastGlobalPrefChange = gFrameCount;
            }
            // if so we've got work to do

            // how much of the frame was spent on non avatar related work?
            U32 non_avatar_time_raw = tot_frame_time_raw - tot_avatar_time_raw;

            // If the target frame time < scene time (estimated as non_avatar time)
            U64 target_avatar_time_raw;
            if(target_frame_time_raw < non_avatar_time_raw)
            {
                // we cannnot do this by avatar adjustment alone.
                if((gFrameCount - FSPerfStats::lastGlobalPrefChange) > settingsChangeFrequency) // give  changes a short time to take effect.
                {
                    if(tunables.userFPSTuningStrategy == TUNE_SCENE_AND_AVATARS)
                    {
                        // 1 - hack the water to opaque. all non opaque have a significant hit, this is a big boost for (arguably) a minor visual hit.
                        // the other reflection options make comparatively little change and if this overshoots we'll be stepping back up later
                        if(LLPipeline::RenderReflectionDetail != -2)
                        {
                            FSPerfStats::tunables.updateReflectionDetail(-2);
                            FSPerfStats::lastGlobalPrefChange = gFrameCount;
                            return;
                        }
                        else // deliberately "else" here so we only do one of these in any given frame
                        {
                            // step down the DD by 10m per update
                            auto new_dd = (LLPipeline::RenderFarClip - DD_STEP > tunables.userMinDrawDistance)?(LLPipeline::RenderFarClip - DD_STEP) : tunables.userMinDrawDistance;
                            if(new_dd != LLPipeline::RenderFarClip)
                            {
                                FSPerfStats::tunables.updateFarClip( new_dd );
                                FSPerfStats::lastGlobalPrefChange = gFrameCount;
                                return;
                            }
                        }
                    }
                    // if we reach here, we've no more changes to make to tune scenery so we'll resort to agressive Avatar tuning
                    // Note: moved from outside "if changefrequency elapsed" to stop fallthrough and allow scenery changes time to take effect.
                    target_avatar_time_raw = 0;
                }
                else 
                {
                    // we made a settings change recently so let's give it time.
                    return;
                }
            }
            else
            {
                // set desired avatar budget.
                target_avatar_time_raw =  target_frame_time_raw - non_avatar_time_raw;
            }

            if( target_avatar_time_raw < tot_avatar_time_raw )
            {
                // we need to spend less time drawing avatars to meet our budget
                auto new_render_limit_ns {FSPerfStats::raw_to_ns(av_render_max_raw)};
                // max render this frame may be higher than the last (cos new entrants and jitter) so make sure we are heading in the right direction
                if( new_render_limit_ns > renderAvatarMaxART_ns )
                {
                    new_render_limit_ns = renderAvatarMaxART_ns;
                }
                new_render_limit_ns -= FSPerfStats::ART_MIN_ADJUST_DOWN_NANOS;

                // bounce at the bottom to prevent "no limit" 
                new_render_limit_ns = std::max((U64)new_render_limit_ns, (U64)FSPerfStats::ART_MINIMUM_NANOS);

                // assign the new value
                if(renderAvatarMaxART_ns != new_render_limit_ns)
                {
                    renderAvatarMaxART_ns = new_render_limit_ns;
                    tunables.updateSettingsFromRenderCostLimit();
                }
                // LL_DEBUGS() << "AUTO_TUNE: avatar_budget adjusted to:" << new_render_limit_ns << LL_ENDL;
            }
            // LL_DEBUGS() << "AUTO_TUNE: Target frame time:"<< FSPerfStats::raw_to_us(target_frame_time_raw) << "usecs (non_avatar is " << FSPerfStats::raw_to_us(non_avatar_time_raw) << "usecs) Max cost limited=" << renderAvatarMaxART_ns << LL_ENDL;
        }
        else if( FSPerfStats::raw_to_ns(target_frame_time_raw) > (FSPerfStats::raw_to_ns(tot_frame_time_raw) + renderAvatarMaxART_ns) )
        {
            if(belowTargetFPS == true)
            {
                // we reached target, force a pause
                lastGlobalPrefChange = gFrameCount;
                belowTargetFPS = false;
            }

            // once we're over the FPS target we slow down further
            if((gFrameCount - lastGlobalPrefChange) > settingsChangeFrequency*3)
            {
                if(!tunables.userAutoTuneLock)
                {
                    // we've reached the target and stayed long enough to consider stable.
                    // turn off if we are not locked.
                    tunables.updateUserAutoTuneEnabled(false);
                }
                if( FSPerfStats::tunedAvatars > 0 )
                {
                    // if we have more time to spare let's shift up little in the hope we'll restore an avatar.
                    renderAvatarMaxART_ns += FSPerfStats::ART_MIN_ADJUST_UP_NANOS;
                    tunables.updateSettingsFromRenderCostLimit();
                    return;
                }
                if(tunables.userFPSTuningStrategy == TUNE_SCENE_AND_AVATARS)
                {
                    if( LLPipeline::RenderFarClip < tunables.userTargetDrawDistance ) 
                    {
                        FSPerfStats::tunables.updateFarClip( std::min(LLPipeline::RenderFarClip + DD_STEP, tunables.userTargetDrawDistance) );
                        FSPerfStats::lastGlobalPrefChange = gFrameCount;
                        return;
                    }
                    if( (tot_frame_time_raw * 1.5) < target_frame_time_raw )
                    {
                        // if everything else is "max" and we have >50% headroom let's knock the water quality up a notch at a time.
                        FSPerfStats::tunables.updateReflectionDetail( std::min(LLPipeline::RenderReflectionDetail + 1, tunables.userTargetReflections) );
                    }
                }
            }
        }
   }
}
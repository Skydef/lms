/*
 * Copyright (C) 2018 Emeric Poupon
 *
 * This file is part of LMS.
 *
 * LMS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * LMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LMS.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "SimilarityFeaturesSearcher.hpp"

#include <random>
#include <unordered_map>

#include "database/Artist.hpp"
#include "database/SimilaritySettings.hpp"
#include "database/Release.hpp"
#include "database/Session.hpp"
#include "database/Track.hpp"
#include "database/TrackFeatures.hpp"
#include "som/DataNormalizer.hpp"
#include "utils/Logger.hpp"
#include "utils/Utils.hpp"


namespace Similarity {

static
std::optional<SOM::InputVector>
getInputVectorFromTrack(Database::Session& session, Database::IdType trackId, const std::unordered_set<FeatureName>& featureNames, std::size_t nbDimensions)
{
	FeatureValuesMap featureValuesMap;

	{
		auto transaction {session.createSharedTransaction()};

		Database::Track::pointer track {Database::Track::getById(session, trackId)};
		if (!track)
			return std::nullopt;

		featureValuesMap = track->getTrackFeatures()->getFeatureValuesMap(featureNames);
		if (featureValuesMap.empty())
			return std::nullopt;
	}

	std::size_t i {};
	std::optional<SOM::InputVector> res {SOM::InputVector {nbDimensions}};
	for (const auto& featureName : featureNames)
	{
		const auto it {featureValuesMap.find(featureName)};
		if (it == std::cend(featureValuesMap))
		{
			LMS_LOG(SIMILARITY, WARNING) << "Cannot find feature '" << featureName << "' for track id'" << trackId << "'";
			res.reset();
			break;
		}

		if (it->second.size() != getFeatureDef(featureName).nbDimensions)
		{
			LMS_LOG(SIMILARITY, WARNING) << "Dimension mismatch for feature '" << featureName << "'. Expected " << getFeatureDef(featureName).nbDimensions << ", got " << it->second.size() << ", trackId = " << trackId;
			res.reset();
			break;
		}

		for (double val : it->second)
			(*res)[i++] = val;
	}

	return res;
}

static
SOM::InputVector
getInputVectorWeights(const FeatureSettingsMap& featureSettingsMap, std::size_t nbDimensions)
{
	SOM::InputVector weights {nbDimensions};
	std::size_t index {};
	for (const auto& [featureName, featureSettings] : featureSettingsMap)
	{
		const std::size_t featureNbDimensions {getFeatureDef(featureName).nbDimensions};

		for (std::size_t i {}; i < featureNbDimensions; ++i)
			weights[index++] = (1. / featureNbDimensions * featureSettings.weight);
	}

	assert(index == nbDimensions);

	return weights;
}

FeaturesSearcher::FeaturesSearcher(Database::Session& session,
		const TrainSettings& trainSettings,
		StopRequestedFunction stopRequested)
{
	LMS_LOG(SIMILARITY, INFO) << "Constructing features searcher...";

	std::unordered_set<FeatureName> featureNames;
	std::transform(std::cbegin(trainSettings.featureSettingsMap), std::cend(trainSettings.featureSettingsMap), std::inserter(featureNames, std::begin(featureNames)),
		[](const auto& itFeatureSetting) { return itFeatureSetting.first; });

	const std::size_t nbDimensions {std::accumulate(std::cbegin(featureNames), std::cend(featureNames), std::size_t {0},
			[](std::size_t sum, const FeatureName& featureName) { return sum + getFeatureDef(featureName).nbDimensions; })};

	LMS_LOG(SIMILARITY, DEBUG) << "Features dimension = " << nbDimensions;

	std::vector<Database::IdType> trackIds;
	{
		auto transaction {session.createSharedTransaction()};

		LMS_LOG(SIMILARITY, DEBUG) << "Getting Tracks with features...";
		trackIds = Database::Track::getAllIdsWithFeatures(session);
		LMS_LOG(SIMILARITY, DEBUG) << "Getting Tracks with features DONE";
	}

	std::vector<SOM::InputVector> samples;
	std::vector<Database::IdType> samplesTrackIds;

	samples.reserve(trackIds.size());
	samplesTrackIds.reserve(trackIds.size());

	LMS_LOG(SIMILARITY, DEBUG) << "Extracting features...";
	for (Database::IdType trackId : trackIds)
	{
		if (stopRequested && stopRequested())
			return;

		std::optional<SOM::InputVector> inputVector {getInputVectorFromTrack(session, trackId, featureNames, nbDimensions)};
		if (!inputVector)
			continue;

		samples.emplace_back(std::move(*inputVector));
		samplesTrackIds.emplace_back(trackId);
	}
	LMS_LOG(SIMILARITY, DEBUG) << "Extracting features DONE";

	if (samples.empty())
	{
		LMS_LOG(SIMILARITY, INFO) << "Nothing to classify!";
		return;
	}

	LMS_LOG(SIMILARITY, DEBUG) << "Normalizing data...";
	SOM::DataNormalizer dataNormalizer {nbDimensions};

	dataNormalizer.computeNormalizationFactors(samples);
	for (auto& sample : samples)
		dataNormalizer.normalizeData(sample);

	SOM::Coordinate size {static_cast<SOM::Coordinate>(std::sqrt(samples.size() / trainSettings.sampleCountPerNeuron))};
	LMS_LOG(SIMILARITY, INFO) << "Found " << samples.size() << " tracks, constructing a " << size << "*" << size << " network";

	SOM::Network network {size, size, nbDimensions};

	SOM::InputVector weights {getInputVectorWeights(trainSettings.featureSettingsMap, nbDimensions)};
	network.setDataWeights(weights);

	auto progressIndicator{[](const auto& iter)
		{
			LMS_LOG(SIMILARITY, DEBUG) << "Current pass = " << iter.idIteration << " / " << iter.iterationCount;
		}};

	LMS_LOG(SIMILARITY, DEBUG) << "Training network...";
	network.train(samples, trainSettings.iterationCount, progressIndicator, stopRequested);
	LMS_LOG(SIMILARITY, DEBUG) << "Training network DONE";

	if (stopRequested && stopRequested())
		return;

	LMS_LOG(SIMILARITY, DEBUG) << "Classifying tracks...";
	std::map<Database::IdType, std::set<SOM::Position>> trackPositions;
	for (std::size_t i {}; i < samples.size(); ++i)
	{
		if (stopRequested && stopRequested())
			return;

		const SOM::Position position {network.getClosestRefVectorPosition(samples[i])};

		trackPositions[samplesTrackIds[i]].insert(position);
	}

	LMS_LOG(SIMILARITY, DEBUG) << "Classifying tracks DONE";

	init(session, std::move(network), std::move(trackPositions), stopRequested);

	LMS_LOG(SIMILARITY, INFO) << "Successfully constructed features searcher";
}

FeaturesSearcher::FeaturesSearcher(Database::Session& session, FeaturesCache cache, StopRequestedFunction stopRequested)
{
	LMS_LOG(SIMILARITY, INFO) << "Constructing features searcher from cache...";

	init(session, std::move(cache._network), std::move(cache._trackPositions), stopRequested);

	LMS_LOG(SIMILARITY, INFO) << "Successfully constructed features searcher from cache";
}

bool
FeaturesSearcher::isValid() const
{
	return _network.get() != nullptr;
}

bool
FeaturesSearcher::isTrackClassified(Database::IdType trackId) const
{
	return (_trackPositions.find(trackId) != _trackPositions.end());
}

bool
FeaturesSearcher::isReleaseClassified(Database::IdType releaseId) const
{
	return (_releasePositions.find(releaseId) != _releasePositions.end());
}

bool
FeaturesSearcher::isArtistClassified(Database::IdType artistId) const
{
	return (_artistPositions.find(artistId) != _artistPositions.end());
}

std::vector<Database::IdType>
FeaturesSearcher::getSimilarTracks(const std::set<Database::IdType>& tracksIds, std::size_t maxCount) const
{
	return getSimilarObjects(tracksIds, _tracksMap, _trackPositions, maxCount);
}

std::vector<Database::IdType>
FeaturesSearcher::getSimilarReleases(Database::IdType releaseId, std::size_t maxCount) const
{
	return getSimilarObjects({releaseId}, _releasesMap, _releasePositions, maxCount);
}

std::vector<Database::IdType>
FeaturesSearcher::getSimilarArtists(Database::IdType artistId, std::size_t maxCount) const
{
	return getSimilarObjects({artistId}, _artistsMap, _artistPositions, maxCount);
}

void
FeaturesSearcher::dump(Database::Session& session, std::ostream& os) const
{
	if (!isValid())
	{
		os << "Invalid searcher" << std::endl;
		return;
	}

	os << "Number of tracks classified: " << _trackPositions.size() << std::endl;
	os << "Network size: " << _network->getWidth() << " * " << _network->getHeight() << std::endl;
	os << "Ref vectors median distance = " << _networkRefVectorsDistanceMedian << std::endl;

	auto transaction {session.createSharedTransaction()};

	for (SOM::Coordinate y {}; y < _network->getHeight(); ++y)
	{
		for (SOM::Coordinate x {}; x < _network->getWidth(); ++x)
		{
			const auto& trackIds {_tracksMap[{x, y}]};

			os << "{" << x << ", " << y << "}";

			if (y > 0)
				os << " - {" << x << ", " << y - 1 << "}: " << _network->getRefVectorsDistance({x, y}, {x, y - 1});
			if (x > 0)
				os << " - {" << x - 1 << ", " << y << "}: " << _network->getRefVectorsDistance({x, y}, {x - 1, y});
			if (y !=  _network->getHeight() - 1)
				os << " - {" << x << ", " << y + 1 << "}: " << _network->getRefVectorsDistance({x, y}, {x, y + 1});
			if (x !=  _network->getWidth() - 1)
				os << " - {" << x + 1 << ", " << y << "}: " << _network->getRefVectorsDistance({x, y}, {x + 1, y});
			os << std::endl;

			for (Database::IdType trackId : trackIds)
			{
				auto track {Database::Track::getById(session, trackId)};
				if (!track)
					continue;

				os << "\t";
				for (auto artist : track->getArtists())
					os << artist->getName() << " - ";
				if (track->getRelease())
					os << track->getRelease()->getName() << " - ";
				os << track->getName() << std::endl;
			}

		}
		os << std::endl;
	}
}

FeaturesCache
FeaturesSearcher::toCache() const
{
	return FeaturesCache{*_network, _trackPositions};
}

void
FeaturesSearcher::init(Database::Session& session,
			SOM::Network network,
			std::map<Database::IdType,
			std::set<SOM::Position>> tracksPosition,
			std::function<bool()> stopRequested)
{
	_networkRefVectorsDistanceMedian = network.computeRefVectorsDistanceMedian();
	LMS_LOG(SIMILARITY, DEBUG) << "Median distance betweend ref vectors = " << _networkRefVectorsDistanceMedian;

	SOM::Coordinate width {network.getWidth()};
	SOM::Coordinate height {network.getHeight()};

	_artistsMap = SOM::Matrix<std::set<Database::IdType>>{width, height};
	_releasesMap = SOM::Matrix<std::set<Database::IdType>>{width, height};
	_tracksMap = SOM::Matrix<std::set<Database::IdType>>{width, height};

	LMS_LOG(SIMILARITY, DEBUG) << "Constructing maps...";

	for (auto itTrackCoord : tracksPosition)
	{
		if (stopRequested && stopRequested())
			return;

		auto transaction {session.createSharedTransaction()};

		Database::IdType trackId {itTrackCoord.first};
		const std::set<SOM::Position>& positionSet {itTrackCoord.second};

		const Database::Track::pointer track {Database::Track::getById(session, trackId)};
		if (!track)
			continue;

		for (const SOM::Position& position : positionSet)
		{
			_tracksMap[position].insert(trackId);
			_trackPositions[trackId].insert(position);

			if (track->getRelease())
			{
				_releasePositions[track->getRelease().id()].insert(position);
				_releasesMap[position].insert(track->getRelease().id());
			}
			for (const auto& artist : track->getArtists())
			{
				_artistPositions[artist.id()].insert(position);
				_artistsMap[position].insert(artist.id());
			}
		}
	}

	_network = std::make_unique<SOM::Network>(std::move(network));

	LMS_LOG(SIMILARITY, DEBUG) << "Constructing maps... DONE";

}

static
std::set<SOM::Position>
getMatchingRefVectorsPosition(const std::set<Database::IdType>& ids, const std::map<Database::IdType, std::set<SOM::Position>>& objectPosition)
{
	std::set<SOM::Position> res;

	if (ids.empty())
		return res;

	for (auto id : ids)
	{
		auto it = objectPosition.find(id);
		if (it == objectPosition.end())
			continue;

		for (const auto& position : it->second)
			res.insert(position);
	}

	return res;
}

static
std::set<Database::IdType>
getObjectsIds(const std::set<SOM::Position>& positionSet, const SOM::Matrix<std::set<Database::IdType>>& objectsMap )
{
	std::set<Database::IdType> res;

	for (const auto& position : positionSet)
	{
		for (auto id : objectsMap.get(position))
			res.insert(id);
	}

	return res;
}

std::vector<Database::IdType>
FeaturesSearcher::getSimilarObjects(const std::set<Database::IdType>& ids,
		const SOM::Matrix<std::set<Database::IdType>>& objectsMap,
		const std::map<Database::IdType, std::set<SOM::Position>>& objectPosition,
		std::size_t maxCount) const
{
	std::vector<Database::IdType> res;

	if (!isValid())
		return res;

	auto now {std::chrono::system_clock::now()};
	std::mt19937 randGenerator{static_cast<std::mt19937::result_type>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count())};

	std::set<SOM::Position> searchedRefVectorsPosition {getMatchingRefVectorsPosition(ids, objectPosition)};
	if (searchedRefVectorsPosition.empty())
		return res;

	while (1)
	{
		std::set<Database::IdType> closestObjectIds {getObjectsIds(searchedRefVectorsPosition, objectsMap)};

		// Remove objects that are already in input or already reported
		for (auto id : ids)
			closestObjectIds.erase(id);

		for (auto id : res)
			closestObjectIds.erase(id);

		{
			std::vector<Database::IdType> objectIdsToAdd {closestObjectIds.begin(), closestObjectIds.end()};
			std::shuffle(objectIdsToAdd.begin(), objectIdsToAdd.end(), randGenerator);
			std::copy(objectIdsToAdd.begin(), objectIdsToAdd.end(), std::back_inserter(res));
		}

		if (res.size() > maxCount)
			res.resize(maxCount);

		if (res.size() == maxCount)
			break;

		// If there is not enough objects, try again with closest neighbour until there is too much distance
		std::optional<SOM::Position> closestRefVectorPosition {_network->getClosestRefVectorPosition(searchedRefVectorsPosition, _networkRefVectorsDistanceMedian * 0.75)};
		if (!closestRefVectorPosition)
			break;

		searchedRefVectorsPosition.insert(*closestRefVectorPosition);
	}

	return res;
}



} // ns Similarity

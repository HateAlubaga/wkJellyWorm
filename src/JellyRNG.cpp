#include "JellyRNG.h"
#include "Lua.h"
#include <sol/sol.hpp>
#include <Windows.h>
#include <ctime>

int JellyRNG::install(SignatureScanner &, module) {
	auto *lua = Lua::getInstance().getState();
	lua->set_function("jellyRandom", &JellyRNG::random);
	lua->set_function("jellyRandomInt", &JellyRNG::randomInt);
	lua->set_function("jellyRngSeed", []() -> unsigned int { return JellyRNG::getSeed(); });
	lua->set_function("jellyRngIsSeeded", &JellyRNG::isSeeded);
	return 0;
}

void JellyRNG::reset() {
	state = 1;
	currentSeed = 0;
	seeded = false;
}

void JellyRNG::seedFromHost() {
	uint32_t s = (uint32_t)GetTickCount() ^ ((uint32_t)time(nullptr) << 16) ^ 0xA5A5u;
	if(s == 0)
		s = 0xC0FFEEu;
	applySeed(s);
}

void JellyRNG::applySeed(uint32_t seed) {
	if(seed == 0)
		seed = 1;
	currentSeed = seed;
	state = seed;
	seeded = true;
}

uint32_t JellyRNG::getSeed() {
	return currentSeed;
}

bool JellyRNG::isSeeded() {
	return seeded;
}

void JellyRNG::writeToJson(nlohmann::json &config) {
	if(!seeded)
		seedFromHost();
	config["rng"] = {
		{"seed", currentSeed},
		{"algo", "lcg"}
	};
}

void JellyRNG::readFromJson(const nlohmann::json &config) {
	if(!config.contains("rng") || !config["rng"].contains("seed"))
		return;
	try {
		applySeed(config["rng"]["seed"].get<uint32_t>());
	} catch(...) {}
}

uint32_t JellyRNG::nextU32() {
	if(!seeded) {
		static bool warned = false;
		if(!warned) {
			warned = true;
			printf("JellyRNG: no synced seed yet — walib.random() returns 0 until host config arrives\n");
		}
		return 0;
	}
	// Numerical Recipes LCG
	state = state * 1664525u + 1013904223u;
	return state;
}

double JellyRNG::random() {
	return (nextU32() >> 8) * (1.0 / 16777216.0);
}

int JellyRNG::randomInt(int min, int max) {
	if(max < min) {
		int t = min;
		min = max;
		max = t;
	}
	if(min == max)
		return min;
	uint32_t range = (uint32_t)(max - min) + 1u;
	return min + (int)(nextU32() % range);
}

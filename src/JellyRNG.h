#ifndef WKJELLYWORM_JELLYRNG_H
#define WKJELLYWORM_JELLYRNG_H

#include <cstdint>
#include <json.hpp>
#include <sigscanner.h>

class JellyRNG {
public:
	static int install(SignatureScanner &, module);

	static void reset();
	static void seedFromHost();
	static void applySeed(uint32_t seed);
	static uint32_t getSeed();
	static bool isSeeded();

	static void writeToJson(nlohmann::json &config);
	static void readFromJson(const nlohmann::json &config);

	static double random();
	static int randomInt(int min, int max);

private:
	static inline uint32_t state = 1;
	static inline uint32_t currentSeed = 0;
	static inline bool seeded = false;

	static uint32_t nextU32();
};

#endif //WKJELLYWORM_JELLYRNG_H

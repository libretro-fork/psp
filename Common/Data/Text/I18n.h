#pragma once

// I18N = I....18..dots.....N = INTERNATIONALIZATION

// Super simple I18N library.
// Just enough to be useful and usable.
// Spits out easy-to-edit utf-8 .INI files.

// As usual, everything is UTF-8. Nothing else allowed.

#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <memory>

#include <retro_atomic.h>
#include <queues/mpsc_stack.h>

#include "Common/Common.h"
#include "Common/File/Path.h"

// Lookups may run on any thread while the language is reloaded: a category is immutable once
// published, and a replaced one is kept until the repo goes away.

class I18NRepo;
class IniFile;
class Section;

// Don't forget to update the string array in the cpp file if you change this.
enum class I18NCat : uint8_t {
	AUDIO = 0,
	CONTROLS,
	CWCHEATS,
	DESKTOPUI,
	DEVELOPER,
	DIALOG,
	ERRORS,  // Can't name it ERROR, clashes with many defines.
	GAME,
	GRAPHICS,
	INSTALLZIP,
	KEYMAPPING,
	MAINMENU,
	MAINSETTINGS,
	MAPPABLECONTROLS,
	NETWORKING,
	PAUSE,
	POSTSHADERS,
	PSPCREDITS,
	MEMSTICK,
	REMOTEISO,
	REPORTING,
	SAVEDATA,
	SCREEN,
	SEARCH,
	STORE,
	SYSINFO,
	SYSTEM,
	TEXTURESHADERS,
	THEMES,
	UI_ELEMENTS,
	VR,
	ACHIEVEMENTS,
	PSPSETTINGS,
	CATEGORY_COUNT,
	NONE = CATEGORY_COUNT,
};

struct I18NEntry {
	I18NEntry(std::string_view t) : text(t), readFlag(false) {}
	I18NEntry() : readFlag(false) {}
	std::string text;
	bool readFlag;
};

class I18NCategory {
public:
	I18NCategory();
	explicit I18NCategory(const Section &section);
	~I18NCategory();

	// Faster since the string lengths don't need to be recomputed.
	std::string_view T(std::string_view key, std::string_view def = "");

	// Try to avoid this. Still useful in snprintf.
	const char *T_cstr(const char *key, const char *def = nullptr);

	// Safe alongside lookups. ClearMissed() must not run alongside Missed().
	std::map<std::string, std::string, std::less<>> Missed() const;

	const std::map<std::string, I18NEntry, std::less<>> &GetMap() { return map_; }
	void ClearMissed();

private:
	void SetMap(const std::map<std::string, std::string> &m);
	void NoteMissed(std::string_view key, std::string_view def);

	// std::less<> is needed to be able to look up string_views in a string-keyed map.
	std::map<std::string, I18NEntry, std::less<>> map_;

	// Missed keys: a lock-free set of key hashes so each is recorded once, and the
	// records themselves on an MPSC stack that is only ever walked, never popped.
	struct MissedKey;
	struct MissedLink {
		mpsc_stack_node_t node;  // first, so a node pointer is a link pointer
		MissedKey *self;
	};
	struct MissedKey {
		MissedLink link;
		std::string key;
		std::string def;
	};
	enum { MISSED_SLOTS = 1024 };
	retro_atomic_ptr_t missedHashes_[MISSED_SLOTS];  // hashes, as pointer-sized words
	mpsc_stack_t missed_;

	std::string name_;
	// Noone else can create these.
	friend class I18NRepo;
};

class I18NRepo {
public:
	I18NRepo();
	bool IniExists(const std::string &languageID) const;
	bool LoadIni(const std::string &languageID, const Path &overridePath = Path()); // NOT the filename!

	std::string LanguageID();

	I18NCategory *GetCategory(I18NCat category);

	// Translate the string, by looking up "key" in the file, and falling back to either def or key, in that order, if the lookup fails.
	// def can (and usually is) set to nullptr.
	std::string_view T(I18NCat category, std::string_view key, std::string_view def = "") {
		if (category == I18NCat::NONE)
			return !def.empty() ? def : key;
		return Cat(category)->T(key, def);
	}
	const char *T_cstr(I18NCat category, const char *key, const char *def = nullptr) {
		if (category == I18NCat::NONE)
			return def ? def : key;
		return Cat(category)->T_cstr(key, def);
	}
	void LogMissingKeys() const;

private:
	Path GetIniPath(const std::string &languageID) const;
	void Clear();
	void Publish(size_t index, I18NCategory *cat);
	I18NCategory *Cat(I18NCat category) const {
		return (I18NCategory *)retro_atomic_load_acquire_ptr(const_cast<retro_atomic_ptr_t *>(&cats_[(size_t)category]));
	}

	retro_atomic_ptr_t cats_[(size_t)I18NCat::CATEGORY_COUNT];
	// Every category ever published, since a lookup may still hold an old one.
	std::vector<std::unique_ptr<I18NCategory>> owned_;
	std::string languageID_;
};

extern I18NRepo g_i18nrepo;

// These are simply talking to the one global instance of I18NRepo.

I18NCategory *GetI18NCategory(I18NCat cat);

inline std::string_view T(I18NCat category, std::string_view key, std::string_view def = "") {
	return g_i18nrepo.T(category, key, def);
}

inline const char *T_cstr(I18NCat category, const char *key, const char *def = "") {
	return g_i18nrepo.T_cstr(category, key, def);
}

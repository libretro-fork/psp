#include <map>
#include <string_view>
#include <cstring>

#include "Common/Data/Text/I18n.h"
#include "Common/Data/Format/IniFile.h"
#include "Common/File/VFS/VFS.h"
#include "Common/Log.h"

#include "Common/StringUtils.h"

// Don't forget to update the constants in the header file if you change this.
static const char * const g_categoryNames[(size_t)I18NCat::CATEGORY_COUNT] = {
	"Audio",
	"Controls",
	"CwCheats",
	"DesktopUI",
	"Developer",
	"Dialog",
	"Error",
	"Game",
	"Graphics",
	"InstallZip",
	"KeyMapping",
	"MainMenu",
	"MainSettings",
	"MappableControls",
	"Networking",
	"Pause",
	"PostShaders",
	"PSPCredits",
	"MemStick",
	"RemoteISO",
	"Reporting",
	"Savedata",
	"Screen",
	"Search",
	"Store",
	"SysInfo",
	"System",
	"TextureShaders",
	"Themes",
	"UI Elements",
	"VR",
	"Achievements",
	"PSPSettings",
};

I18NRepo g_i18nrepo;

std::string I18NRepo::LanguageID() {
	return languageID_;
}

I18NRepo::I18NRepo() {
	for (size_t i = 0; i < (size_t)I18NCat::CATEGORY_COUNT; i++)
		retro_atomic_ptr_init(&cats_[i], nullptr);
	Clear();
}

void I18NRepo::Publish(size_t index, I18NCategory *cat) {
	owned_.emplace_back(cat);
	retro_atomic_store_release_ptr(&cats_[index], cat);
}

void I18NRepo::Clear() {
	for (size_t i = 0; i < (size_t)I18NCat::CATEGORY_COUNT; i++) {
		// Initialize with empty categories, so that early lookups don't crash.
		Publish(i, new I18NCategory());
	}
}

I18NCategory::I18NCategory() {
	for (int i = 0; i < MISSED_SLOTS; i++)
		retro_atomic_ptr_init(&missedHashes_[i], nullptr);
	mpsc_stack_init(&missed_);
}

I18NCategory::I18NCategory(const Section &section) : I18NCategory() {
	std::map<std::string, std::string> sectionMap = section.ToMap();
	SetMap(sectionMap);
	name_ = section.name().c_str();
}

I18NCategory::~I18NCategory() {
	ClearMissed();
}

void I18NCategory::ClearMissed() {
	mpsc_stack_node_t *node = mpsc_stack_drain(&missed_);
	while (node) {
		mpsc_stack_node_t *next = node->next;
		delete ((MissedLink *)node)->self;
		node = next;
	}
	for (int i = 0; i < MISSED_SLOTS; i++)
		retro_atomic_store_relaxed_ptr(&missedHashes_[i], nullptr);
}

void I18NCategory::NoteMissed(std::string_view key, std::string_view def) {
	// FNV-1a, never zero, which marks a free slot.
	uintptr_t h = (uintptr_t)14695981039346656037ULL;
	for (char c : key)
		h = (h ^ (unsigned char)c) * (uintptr_t)1099511628211ULL;
	if (h == 0)
		h = 1;
	for (int probe = 0; probe < MISSED_SLOTS; probe++) {
		retro_atomic_ptr_t *slot = &missedHashes_[(h + probe) & (MISSED_SLOTS - 1)];
		const uintptr_t seen = (uintptr_t)retro_atomic_load_acquire_ptr(slot);
		if (seen == h)
			return;  // already recorded
		if (seen == 0) {
			if (!retro_atomic_cas_ptr(slot, nullptr, (void *)h)) {
				if ((uintptr_t)retro_atomic_load_acquire_ptr(slot) == h)
					return;
				continue;
			}
			MissedKey *missed = new MissedKey{ {}, std::string(key), std::string(!def.empty() ? def : key) };
			missed->link.self = missed;
			mpsc_stack_push(&missed_, &missed->link.node);
			return;
		}
	}
	// Full: stop recording.
}

std::string_view I18NCategory::T(std::string_view key, std::string_view def) {
	auto iter = map_.find(key);
	if (iter != map_.end()) {
		return iter->second.text.c_str();
	} else {
		if (map_.empty()) {
			// Too early. This is probably in desktop-ui translation.
			return !def.empty() ? def : key;
		}
		if (key != "Font") {
			// Font is allowed to be missing.
			DEBUG_LOG(Log::UI, "Missing translation [%s] %.*s (%.*s)", name_.c_str(), STR_VIEW(key), STR_VIEW(def));
			NoteMissed(key, def);
		}
		return !def.empty() ? def : key;
	}
}

const char *I18NCategory::T_cstr(const char *key, const char *def) {
	auto iter = map_.find(key);
	if (iter != map_.end()) {
		return iter->second.text.c_str();
	} else {
		if (map_.empty()) {
			// Too early. This is probably in desktop-ui translation.
			return def ? def : key;
		}
		if (strcmp(key, "Font") != 0) {
			DEBUG_LOG(Log::UI, "Missing translation %s (%s)", key, def);
			NoteMissed(key, def ? def : "");
		}
		return def ? def : key;
	}
}

void I18NCategory::SetMap(const std::map<std::string, std::string> &m) {
	for (const auto &[key, value] : m) {
		if (map_.find(key) == map_.end()) {
			std::string text = ReplaceAll(value, "\\n", "\n");
			_dbg_assert_(key.find('\n') == std::string::npos);
			map_[key] = I18NEntry(text);
		}
	}
}

std::map<std::string, std::string, std::less<>> I18NCategory::Missed() const {
	// Pushes only add in front of what is already there, so the walk is safe.
	std::map<std::string, std::string, std::less<>> missed;
	const mpsc_stack_node_t *node = (const mpsc_stack_node_t *)retro_atomic_load_acquire_ptr(const_cast<retro_atomic_ptr_t *>(&missed_.head));
	for (; node; node = node->next) {
		const MissedKey *m = ((const MissedLink *)node)->self;
		missed[m->key] = m->def;
	}
	return missed;
}

I18NCategory *I18NRepo::GetCategory(I18NCat category) {
	if (category != I18NCat::NONE)
		return Cat(category);
	else
		return nullptr;
}

Path I18NRepo::GetIniPath(const std::string &languageID) const {
	return Path("lang") / (languageID + ".ini");
}

bool I18NRepo::IniExists(const std::string &languageID) const {
	File::FileInfo info;
	if (!g_VFS.Exists(GetIniPath(languageID).ToString().c_str()))
		return false;
	return true;
}

bool I18NRepo::LoadIni(const std::string &languageID, const Path &overridePath) {
	IniFile ini;
	Path iniPath;

//	INFO_LOG(Log::UI, "Loading lang ini %s", iniPath.c_str());
	if (!overridePath.empty()) {
		iniPath = overridePath / (languageID + ".ini");
	} else {
		iniPath = GetIniPath(languageID);
	}

	if (!ini.LoadFromVFS(g_VFS, iniPath.ToString()))
		return false;

	// Build every category first, then publish: a lookup sees either language, whole.
	const std::vector<std::unique_ptr<Section>> &sections = ini.Sections();
	I18NCategory *built[(size_t)I18NCat::CATEGORY_COUNT]{};
	for (auto &section : sections) {
		for (size_t i = 0; i < (size_t)I18NCat::CATEGORY_COUNT; i++) {
			if (!strcmp(section->name().c_str(), g_categoryNames[i])) {
				delete built[i];
				built[i] = new I18NCategory(*section.get());
			}
		}
	}
	for (size_t i = 0; i < (size_t)I18NCat::CATEGORY_COUNT; i++)
		Publish(i, built[i] ? built[i] : new I18NCategory());

	languageID_ = languageID;
	return true;
}

void I18NRepo::LogMissingKeys() const {
	for (size_t i = 0; i < (size_t)I18NCat::CATEGORY_COUNT; i++) {
		const I18NCategory *cat = Cat((I18NCat)i);
		for (auto &key : cat->Missed()) {
			INFO_LOG(Log::UI, "Missing translation [%s]: %s (%s)", g_categoryNames[i], key.first.c_str(), key.second.c_str());
		}
	}
}

I18NCategory *GetI18NCategory(I18NCat category) {
	if (category == I18NCat::NONE) {
		return nullptr;
	}
	I18NCategory *cat = g_i18nrepo.GetCategory(category);
	_dbg_assert_(cat);
	return cat;
}

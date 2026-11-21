#include "HFRStrings.hpp"

#include <QSettings>

namespace hfrt {

static Lang g_lang = Lang::Zh;
static bool g_loaded = false;

static void EnsureLoaded()
{
	if (g_loaded) {
		return;
	}
	g_loaded = true;
	QSettings s(QStringLiteral("HuafeiRongOBS"), QStringLiteral("ui"));
	g_lang = (s.value(QStringLiteral("lang"), 0).toInt() == 1) ? Lang::En : Lang::Zh;
}

Lang GetLang()
{
	EnsureLoaded();
	return g_lang;
}

void SetLang(Lang lang)
{
	g_lang = lang;
	g_loaded = true;
	QSettings s(QStringLiteral("HuafeiRongOBS"), QStringLiteral("ui"));
	s.setValue(QStringLiteral("lang"), lang == Lang::En ? 1 : 0);
	s.sync();
}

QString T(const char *key, const char *zh, const char *en)
{
	Q_UNUSED(key);
	EnsureLoaded();
	if (g_lang == Lang::En) {
		return QString::fromUtf8(en);
	}
	return QString::fromUtf8(zh);
}

} // namespace hfrt

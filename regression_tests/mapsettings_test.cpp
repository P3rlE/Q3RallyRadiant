#include <QApplication>
#include <QFontDatabase>
#include <QFont>
#include <QTimer>
#include <QAbstractButton>
#include "q3rally_mapsettings.h"
#include <cassert>
#include <cstdlib>
class MockEntity : public Entity {
public:
    Q3RallyMapValues values;
    const EntityClass &getEntityClass() const override { std::abort(); }
    const char *getClassName() const override { return "worldspawn"; }
    void forEachKeyValue(Visitor &v) const override { for(auto i=values.cbegin();i!=values.cend();++i) v.visit(i.key().toUtf8().constData(), i.value().toUtf8().constData()); }
    void setKeyValue(const char *k,const char *v) override { values[k]=v; }
    const char *getKeyValue(const char *k) const override { return ""; }
    bool hasKeyValue(const char *k) const override { return values.contains(k); }
    bool isContainer() const override { return true; }
    void attach(Observer &) override {}
    void detach(Observer &) override {}
};
static void click(QDialogButtonBox::StandardButton button) {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); assert(dialog);
    auto *buttons=dialog->findChild<QDialogButtonBox *>(); assert(buttons);
    buttons->button(button)->click();
}
int main(int argc,char **argv) {
    QApplication app(argc,argv);
    int font = QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf");
    if(font>=0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).first(),10));
    const Q3RallyWorldspawnField chance={"Weather","weatherChance","40","chance",""};
    assert(Q3Rally_ValidWorldspawnValue(chance,"0")); assert(Q3Rally_ValidWorldspawnValue(chance,"100"));
    assert(!Q3Rally_ValidWorldspawnValue(chance,"-1")); assert(!Q3Rally_ValidWorldspawnValue(chance,"nan"));
    const Q3RallyWorldspawnField wind={"Weather","weatherWind","30 250","wind",""};
    assert(Q3Rally_ValidWorldspawnValue(wind,"0 600")); assert(!Q3Rally_ValidWorldspawnValue(wind,"250 30"));
    assert(!Q3Rally_ValidWorldspawnValue(wind,"0 601"));
    Q3RallyMapValues result; result["sentinel"]="untouched";
    QTimer::singleShot(0,[](){click(QDialogButtonBox::Cancel);});
    assert(!Q3Rally_EditMapSettings(nullptr,nullptr,true,result)); assert(result.value("sentinel")=="untouched");
    QTimer::singleShot(0,[&](){
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if(argc>1) dialog->grab().save(argv[1]);
        click(QDialogButtonBox::Ok);
    });
    assert(Q3Rally_EditMapSettings(nullptr,nullptr,true,result));
    assert(result.value("gravity")=="1400" && result.value("allowShortTrack")=="1");
    assert(result.value("weatherChance")=="40" && !result.contains("weather") && !result.contains("_ambient"));
    MockEntity entity;
    entity.values["message"]="Existing"; entity.values["myCustomKey"]="preserved";
    entity.values["allowShortTrack"]=entity.values["allowMediumTrack"]=entity.values["allowLongTrack"]="0";
    entity.values["weatherChance"]="200"; // unchanged legacy value must survive
    QTimer::singleShot(0,[](){click(QDialogButtonBox::Ok);});
    assert(Q3Rally_EditMapSettings(nullptr,&entity,false,result)); assert(result==entity.values);
    QTimer::singleShot(0,[](){
        auto *dialog = QApplication::activeModalWidget();
        for(auto *edit : dialog->findChildren<QLineEdit *>()) if(edit->text()=="Existing") edit->setText("Renamed map");
        click(QDialogButtonBox::Ok);
    });
    assert(Q3Rally_EditMapSettings(nullptr,&entity,false,result));
    assert(result.value("message")=="Renamed map" && result.value("myCustomKey")=="preserved");
    puts("Worldspawn dialog tests passed");
}

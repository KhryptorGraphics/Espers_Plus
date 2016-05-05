#include "overviewpage.h"
#include "ui_overviewpage.h"

#include "clientmodel.h"
#include "walletmodel.h"
#include "bitcoinunits.h"
#include "optionsmodel.h"
#include "transactiontablemodel.h"
#include "transactionfilterproxy.h"
#include "guiutil.h"
#include "guiconstants.h"
// For mining function
#include "init.h"

#include <QAbstractItemDelegate>
#include <QPainter>
#include <QDesktopServices>
#include <QUrl>
// for hashmeter
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QMessageBox>
#include <QTextStream>
#include <QProcess>
#include <QString>

#include <QStandardPaths>

#define DECORATION_SIZE 64
#define NUM_ITEMS 3

class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    TxViewDelegate(): QAbstractItemDelegate(), unit(BitcoinUnits::ESP)
    {

    }

    inline void paint(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index ) const
    {
        painter->save();

        QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        QRect mainRect = option.rect;
        QRect decorationRect(mainRect.topLeft(), QSize(DECORATION_SIZE, DECORATION_SIZE));
        int xspace = DECORATION_SIZE + 8;
        int ypad = 6;
        int halfheight = (mainRect.height() - 2*ypad)/2;
        QRect amountRect(mainRect.left() + xspace, mainRect.top()+ypad, mainRect.width() - xspace, halfheight);
        QRect addressRect(mainRect.left() + xspace, mainRect.top()+ypad+halfheight, mainRect.width() - xspace, halfheight);
        icon.paint(painter, decorationRect);

        QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        QString address = index.data(Qt::DisplayRole).toString();
        qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();
        QVariant value = index.data(Qt::ForegroundRole);
        QColor foreground = option.palette.color(QPalette::Text);
        if(value.canConvert<QBrush>())
        {
            QBrush brush = qvariant_cast<QBrush>(value);
            foreground = brush.color();
        }

        painter->setPen(foreground);
        painter->drawText(addressRect, Qt::AlignLeft|Qt::AlignVCenter, address);

        if(amount < 0)
        {
            foreground = COLOR_NEGATIVE;
        }
        else if(!confirmed)
        {
            foreground = COLOR_UNCONFIRMED;
        }
        else
        {
            foreground = option.palette.color(QPalette::Text);
        }
        painter->setPen(foreground);
        QString amountText = BitcoinUnits::formatWithUnit(unit, amount, true);
        if(!confirmed)
        {
            amountText = QString("[") + amountText + QString("]");
        }
        painter->drawText(amountRect, Qt::AlignRight|Qt::AlignVCenter, amountText);

        painter->setPen(option.palette.color(QPalette::Text));
        painter->drawText(amountRect, Qt::AlignLeft|Qt::AlignVCenter, GUIUtil::dateTimeStr(date));

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        return QSize(DECORATION_SIZE, DECORATION_SIZE);
    }

    int unit;

};
#include "overviewpage.moc"

OverviewPage::OverviewPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OverviewPage),
    clientModel(0),
    walletModel(0),
    currentBalance(-1),
    currentUnconfirmedBalance(-1),
    currentImmatureBalance(-1),
    txdelegate(new TxViewDelegate()),
    filter(0)
{
    ui->setupUi(this);

    // Recent transactions
    ui->listTransactions->setItemDelegate(txdelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);

    connect(ui->listTransactions, SIGNAL(clicked(QModelIndex)), this, SLOT(handleTransactionClicked(QModelIndex)));

    // init "out of sync" warning labels
    ui->labelWalletStatus->setText("(" + tr("out of sync") + ")");
    ui->labelTransactionsStatus->setText("(" + tr("out of sync") + ")");

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);
}

void OverviewPage::handleTransactionClicked(const QModelIndex &index)
{
    if(filter)
        emit transactionClicked(filter->mapToSource(index));
}

OverviewPage::~OverviewPage()
{
    delete ui;
}

void OverviewPage::setBalance(qint64 balance, qint64 unconfirmedBalance, qint64 immatureBalance)
{
    int unit = walletModel->getOptionsModel()->getDisplayUnit();
    currentBalance = balance;
    currentUnconfirmedBalance = unconfirmedBalance;
    currentImmatureBalance = immatureBalance;
    ui->labelBalance->setText(BitcoinUnits::formatWithUnit(unit, balance));
    ui->labelUnconfirmed->setText(BitcoinUnits::formatWithUnit(unit, unconfirmedBalance));
    ui->labelImmature->setText(BitcoinUnits::formatWithUnit(unit, immatureBalance));

    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool showImmature = immatureBalance != 0;
    ui->labelImmature->setVisible(showImmature);
    ui->labelImmatureText->setVisible(showImmature);
}

void OverviewPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if(model)
    {
        // Show warning if this is a prerelease version
        connect(model, SIGNAL(alertsChanged(QString)), this, SLOT(updateAlerts(QString)));
        updateAlerts(model->getStatusBarWarnings());
    }
}

void OverviewPage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
    if(model && model->getOptionsModel())
    {
        // Set up transaction list
        filter = new TransactionFilterProxy();
        filter->setSourceModel(model->getTransactionTableModel());
        filter->setLimit(NUM_ITEMS);
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->sort(TransactionTableModel::Status, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter);
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);

        // Keep up to date with wallet
        setBalance(model->getBalance(), model->getUnconfirmedBalance(), model->getImmatureBalance());
        connect(model, SIGNAL(balanceChanged(qint64, qint64, qint64)), this, SLOT(setBalance(qint64, qint64, qint64)));

        connect(model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));
    }

    // update the display unit, to not use the default ("BTC")
    updateDisplayUnit();
}

void OverviewPage::updateDisplayUnit()
{
    if(walletModel && walletModel->getOptionsModel())
    {
        if(currentBalance != -1)
            setBalance(currentBalance, currentUnconfirmedBalance, currentImmatureBalance);

        // Update txdelegate->unit with the current unit
        txdelegate->unit = walletModel->getOptionsModel()->getDisplayUnit();

        ui->listTransactions->update();
    }
}

void OverviewPage::updateAlerts(const QString &warnings)
{
    this->ui->labelAlerts->setVisible(!warnings.isEmpty());
    this->ui->labelAlerts->setText(warnings);
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
}

void OverviewPage::on_pushButton_clicked()
{

    QString link="http://cryptocoderz.com/";
    QDesktopServices::openUrl(QUrl(link));
}

void OverviewPage::on_pushButton_2_clicked()
{

    QString link="https://bitcointalk.org/";
    QDesktopServices::openUrl(QUrl(link));
}

void OverviewPage::on_pushButton_3_clicked()
{

    QPixmap pix(":/images/res/images/ESP/ESP_Full.png");
    this->ui->label_wallet_bgcoin->setPixmap(pix);
}

void OverviewPage::on_pushButton_4_clicked()
{
    QPixmap pix(":/images/res/images/ESP/ESP.png");
    this->ui->label_wallet_bgcoin->setPixmap(pix);
}


void OverviewPage::on_pushButton_5_clicked()
{
    QString link="http://bitcoingarden.tk/forum/index.php?topic=7402.0";
    QDesktopServices::openUrl(QUrl(link));
}

void OverviewPage::on_pushButton_6_clicked()
{
    QString link="http://esp.miningalts.com/";
    QDesktopServices::openUrl(QUrl(link));
}

// Mining Button
int sMine = 0; // Change text per click
// Actual function
void OverviewPage::on_pushButton_7_clicked()
{
    if (sMine < 1)
    {
        this->ui->pushButton_7->setText("Stop Mining");
        sMine ++;
        GenerateBitcoins(GetBoolArg("-gen", true), pwalletMain);       
        // PreCreate Hashmeter files
        // Hash/s
        boost::filesystem::path meterPath;
        meterPath = GetDefaultDataDir() / "Hashes.sec";
        FILE* meterFile = fopen(meterPath.string().c_str(), "w");
        fprintf(meterFile, "0 Hash/s\n");
        fclose(meterFile);
        // kHash/s
        boost::filesystem::path meter2Path;
        meter2Path = GetDefaultDataDir() / "kHashes.sec";
        FILE* meter2File = fopen(meter2Path.string().c_str(), "w");
        fprintf(meter2File, "0 Hash/s\n");
        fclose(meter2File);
        // mHash/s
        boost::filesystem::path meter3Path;
        meter3Path = GetDefaultDataDir() / "mHashes.sec";
        FILE* meter3File = fopen(meter3Path.string().c_str(), "w");
        fprintf(meter3File, "0 Hash/s\n");
        fclose(meter3File);

    }

    else if (sMine >= 1)
    {
        this->ui->pushButton_7->setText("Start Mining");
        sMine --;
        GenerateBitcoins(GetBoolArg("-gen", false), pwalletMain);
        // Set Hashmeter to Zero
        // Hash/s check
        if (dHashesPerSec < 1000)
        {
            boost::filesystem::path ConfPath;
            ConfPath = GetDefaultDataDir() / "Hashes.sec";
            FILE* ConfFile = fopen(ConfPath.string().c_str(), "w");
            fprintf(ConfFile, "0 Hash/s\n");
            fclose(ConfFile);
        }
        // kHash/s check
        else if (dHashesPerSec > 999)
        {
            boost::filesystem::path ConfPath;
            ConfPath = GetDefaultDataDir() / "kHashes.sec";
            FILE* ConfFile = fopen(ConfPath.string().c_str(), "w");
            fprintf(ConfFile, "0 Hash/s\n");
            fclose(ConfFile);
        }
        // mHash/s check
        else if (dHashesPerSec > 999999)
        {
            boost::filesystem::path ConfPath;
            ConfPath = GetDefaultDataDir() / "mHashes.sec";
            FILE* ConfFile = fopen(ConfPath.string().c_str(), "w");
            fprintf(ConfFile, "0 Hash/s\n");
            fclose(ConfFile);
        }
    }

}

void OverviewPage::on_pushButton_8_clicked()
{
    // Hashmeter-------
    // Hash/s check
    if (dHashesPerSec < 1000)
    {
        //
#ifdef Q_OS_LINUX
		QString curtxt = QDir::homePath() + "/.Espers/Hashes.sec";
#elif defined(Q_OS_MAC)
        QString curtxt = QDir::homePath() + QDir::toNativeSeparators("/Library/Application Support/Espers/Hashes.sec");
#else // Windows
        QString curtxt = QDir::homePath() + "/AppData/Roaming/Espers/Hashes.sec";
#endif 
        QFile fileV(curtxt);
        if(!fileV.open(QIODevice::ReadOnly | QIODevice::Text))

            // error out if not accesable
            QMessageBox::information(0,"info",fileV.errorString());

        QTextStream inV(&fileV);
        this->ui->c_hashrate->setPlainText(inV.readAll());
    }
    // kHash/s check
    else if (dHashesPerSec > 999)
    {
        //
#ifdef Q_OS_LINUX
		QString curtxt = QDir::homePath() + "/.Espers/kHashes.sec";
#elif defined(Q_OS_MAC)
        QString curtxt = QDir::homePath() + QDir::toNativeSeparators("/Library/Application Support/Espers/kHashes.sec");
#else //Windows
        QString curtxt = QDir::homePath() + "/AppData/Roaming/Espers/kHashes.sec";
#endif        
        QFile fileV(curtxt);
        if(!fileV.open(QIODevice::ReadOnly | QIODevice::Text))

            // error out if not accesable
            QMessageBox::information(0,"info",fileV.errorString());

        QTextStream inV(&fileV);
        this->ui->c_hashrate->setPlainText(inV.readAll());
    }
    // mHash/s check
    else if (dHashesPerSec > 999999)
    {
        //
#ifdef Q_OS_LINUX
		QString curtxt = QDir::homePath() + "/.Espers/mHashes.sec";
#elif defined(Q_OS_MAC)
        QString curtxt = QDir::homePath() +  QDir::toNativeSeparators("/Library/Application Support/Espers/mHashes.sec");
#else //Windows
        QString curtxt = QDir::homePath() + "/AppData/Roaming/Espers/mHashes.sec";
#endif    
        QFile fileV(curtxt);
        if(!fileV.open(QIODevice::ReadOnly | QIODevice::Text))

            // error out if not accesable
            QMessageBox::information(0,"info",fileV.errorString());

        QTextStream inV(&fileV);
        this->ui->c_hashrate->setPlainText(inV.readAll());
    }

}

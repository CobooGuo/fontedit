#include "mainwindow.h"
#include "f2b.h"
#include "facewidget.h"
#include "fontfaceviewmodel.h"
#include "command.h"

#include <QGraphicsGridLayout>
#include <QGraphicsWidget>
#include <QDebug>
#include <QStyle>
#include <QFontDialog>
#include <QFileDialog>
#include <QScrollBar>
#include <QMessageBox>
#include <QKeySequence>
#include <QElapsedTimer>
#include <QStandardPaths>

#include <iostream>
#include <stdexcept>

#include <QFile>
#include <QTextStream>

static constexpr auto editTabIndex = 0;
static constexpr auto codeTabIndex = 1;
static constexpr auto fileFilter = "FontEdit documents (*.fontedit)";

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    ui_->setupUi(this);

    initUI();
    setupActions();
    updateUI(viewModel_->uiState());

    connectUIInputs();
    connectViewModelOutputs();
    viewModel_->restoreSession();
}

void MainWindow::connectUIInputs()
{
    connect(ui_->actionImport_Font, &QAction::triggered, this, &MainWindow::showFontDialog);
    connect(ui_->actionOpen, &QAction::triggered, this, &MainWindow::showOpenFaceDialog);
    connect(ui_->actionReset_Glyph, &QAction::triggered, this, &MainWindow::resetCurrentGlyph);
    connect(ui_->actionReset_Font, &QAction::triggered, this, &MainWindow::resetFont);

    connect(ui_->actionSave, &QAction::triggered, this, &MainWindow::save);
    connect(ui_->actionSave_As, &QAction::triggered, this, &MainWindow::saveAs);
    connect(ui_->actionClose, &QAction::triggered, viewModel_.get(), &MainWindowModel::closeCurrentDocument);

    connect(ui_->actionExport, &QAction::triggered, this, &MainWindow::exportSourceCode);
    connect(ui_->exportButton, &QPushButton::clicked, this, &MainWindow::exportSourceCode);

    connect(ui_->actionQuit, &QAction::triggered, this, &MainWindow::close);
    connect(ui_->tabWidget, &QTabWidget::currentChanged, [&](int index) {
        if (index == codeTabIndex) {
            viewModel_->prepareSourceCodeTab();
        }
    });
    connect(ui_->invertBitsCheckBox, &QCheckBox::stateChanged, [&](int state) {
        viewModel_->setInvertBits(state == Qt::Checked);
    });
    connect(ui_->bitNumberingCheckBox, &QCheckBox::stateChanged, [&](int state) {
        viewModel_->setMSBEnabled(state == Qt::Checked);
    });
    connect(ui_->lineSpacingCheckBox, &QCheckBox::stateChanged, [&](int state) {
        viewModel_->setIncludeLineSpacing(state == Qt::Checked);
    });
    connect(ui_->formatComboBox, &QComboBox::currentTextChanged,
            viewModel_.get(), &MainWindowModel::setOutputFormat);
}

void MainWindow::connectViewModelOutputs()
{
    connect(viewModel_.get(), &MainWindowModel::documentTitleChanged, [&](const QString& title) {
        ui_->tabWidget->setTabText(editTabIndex, title);
    });
    connect(viewModel_.get(), &MainWindowModel::uiStateChanged, this, &MainWindow::updateUI);
    connect(viewModel_.get(), &MainWindowModel::faceLoaded, this, &MainWindow::displayFace);
    connect(viewModel_.get(), &MainWindowModel::documentError, this, &MainWindow::displayError);
    connect(viewModel_.get(), &MainWindowModel::activeGlyphChanged, this, &MainWindow::displayGlyph);
    connect(viewModel_.get(), &MainWindowModel::sourceCodeUpdating, [&]() {
//        ui_->stackedWidget->setCurrentWidget(ui_->spinnerContainer);
    });
    connect(viewModel_.get(), &MainWindowModel::sourceCodeChanged, [&](const QString& text) {
        ui_->stackedWidget->setCurrentWidget(ui_->sourceCodeContainer);
        ui_->sourceCodeTextBrowser->setPlainText(text);
    });
    connect(viewModel_.get(), &MainWindowModel::documentClosed, this, &MainWindow::closeCurrentDocument);
}

void MainWindow::initUI()
{
    faceScene_->setBackgroundBrush(QBrush(Qt::lightGray));
    ui_->faceGraphicsView->setScene(faceScene_.get());

    ui_->faceInfoLabel->setVisible(false);

    auto scrollBarWidth = ui_->faceGraphicsView->verticalScrollBar()->sizeHint().width();
    auto faceViewWidth = static_cast<int>(FaceWidget::cell_width) * 3 + scrollBarWidth;
    ui_->faceGraphicsView->setMinimumSize({ faceViewWidth,
                                            ui_->faceGraphicsView->minimumSize().height() });

    ui_->invertBitsCheckBox->setCheckState(viewModel_->invertBits());
    ui_->bitNumberingCheckBox->setCheckState(viewModel_->msbEnabled());
    ui_->lineSpacingCheckBox->setCheckState(viewModel_->includeLineSpacing());

    for (const auto &[identifier, name] : viewModel_->outputFormats().toStdMap()) {
        ui_->formatComboBox->addItem(name, identifier);
    }
    ui_->formatComboBox->setCurrentText(viewModel_->outputFormat());

    QFont f("Monaco", 13);
    f.setStyleHint(QFont::TypeWriter);
    ui_->sourceCodeTextBrowser->setFont(f);
}

void MainWindow::closeCurrentDocument()
{
    ui_->faceInfoLabel->setVisible(false);

    if (faceWidget_ != nullptr) {
        faceScene_->removeItem(faceWidget_);
        delete faceWidget_;
        faceWidget_ = nullptr;
    }

    if (auto g = glyphWidget_.get()) {
        ui_->glyphGraphicsView->scene()->removeItem(g);
        glyphWidget_.release();
    }
}

void MainWindow::setupActions()
{
    auto undo = undoStack_->createUndoAction(this);
    undo->setIcon(QIcon {":/toolbar/assets/undo.svg"});
    undo->setShortcut(QKeySequence::Undo);

    auto redo = undoStack_->createRedoAction(this);
    redo->setIcon(QIcon {":/toolbar/assets/redo.svg"});
    redo->setShortcut(QKeySequence::Redo);

    ui_->openButton->setDefaultAction(ui_->actionOpen);
    ui_->importFontButton->setDefaultAction(ui_->actionImport_Font);
    ui_->addGlyphButton->setDefaultAction(ui_->actionAdd_Glyph);
    ui_->saveButton->setDefaultAction(ui_->actionSave);
    ui_->copyButton->setDefaultAction(ui_->actionCopy_Glyph);
    ui_->pasteButton->setDefaultAction(ui_->actionPaste_Glyph);
    ui_->undoButton->setDefaultAction(undo);
    ui_->redoButton->setDefaultAction(redo);
    ui_->resetGlyphButton->setDefaultAction(ui_->actionReset_Glyph);
    ui_->resetFontButton->setDefaultAction(ui_->actionReset_Font);
    ui_->actionReset_Glyph->setEnabled(false);
    ui_->actionReset_Font->setEnabled(false);

    ui_->menuEdit->insertAction(ui_->actionCopy_Glyph, undo);
    ui_->menuEdit->insertAction(ui_->actionCopy_Glyph, redo);
}

void MainWindow::updateUI(MainWindowModel::UIState uiState)
{
    ui_->tabWidget->setTabEnabled(1, uiState[MainWindowModel::InterfaceAction::ActionTabCode]);
    ui_->actionAdd_Glyph->setEnabled(uiState[MainWindowModel::InterfaceAction::ActionAddGlyph]);
    ui_->actionSave->setEnabled(uiState[MainWindowModel::InterfaceAction::ActionSave]);
    ui_->actionSave_As->setEnabled(uiState[MainWindowModel::InterfaceAction::ActionSave]);
    ui_->actionClose->setEnabled(uiState[MainWindowModel::InterfaceAction::ActionClose]);
    ui_->actionCopy_Glyph->setEnabled(uiState[MainWindowModel::InterfaceAction::ActionCopy]);
    ui_->actionPaste_Glyph->setEnabled(uiState[MainWindowModel::InterfaceAction::ActionPaste]);
    ui_->actionExport->setEnabled(uiState[MainWindowModel::InterfaceAction::ActionExport]);
    ui_->actionPrint->setEnabled(uiState[MainWindowModel::InterfaceAction::ActionPrint]);
}

void MainWindow::showFontDialog()
{
    switch (promptToSaveDirtyDocument()) {
    case Save:
        save();
    case DontSave:
        break;
    case Cancel:
        return;
    }

    bool ok;
    QFont f("Monaco", 24);
    f.setStyleHint(QFont::TypeWriter);
    f = QFontDialog::getFont(&ok, f, this, tr("Select Font"), QFontDialog::MonospacedFonts | QFontDialog::DontUseNativeDialog);

    if (ok) {
        qDebug() << "selected font:" << f;
        viewModel_->importFont(f);
    }
}

void MainWindow::showOpenFaceDialog()
{
    switch (promptToSaveDirtyDocument()) {
    case Save:
        save();
    case DontSave:
        break;
    case Cancel:
        return;
    }

    QString fileName = QFileDialog::getOpenFileName(this, tr("Open Document"), QString(), tr(fileFilter));

    if (!fileName.isNull())
        viewModel_->openDocument(fileName);
}

void MainWindow::save()
{
    auto currentPath = viewModel_->currentDocumentPath();
    if (currentPath.has_value()) {
        viewModel_->saveDocument(currentPath.value());
    } else {
        saveAs();
    }
}

void MainWindow::saveAs()
{
    QString directoryPath;
    if (viewModel_->currentDocumentPath().has_value()) {
        directoryPath = QFileInfo(viewModel_->currentDocumentPath().value()).path();
    } else {
        directoryPath = QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation).last();
    }
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save Document"), directoryPath, tr(fileFilter));

    if (!fileName.isNull())
        viewModel_->saveDocument(fileName);
}

void MainWindow::displayError(const QString &error)
{
    QMessageBox::critical(this, tr("Error"), error, QMessageBox::StandardButton::Ok);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    switch (promptToSaveDirtyDocument()) {
    case Save:
        save();
    case DontSave:
        event->accept();
        break;
    case Cancel:
        event->ignore();
    }
}

MainWindow::SavePromptButton MainWindow::promptToSaveDirtyDocument()
{
    if (viewModel_->faceModel() == nullptr || !viewModel_->faceModel()->isModifiedSinceSave()) {
        return DontSave; // ignore this dialog and move on
    }

    QStringList buttons { tr("Save"), tr("Don't Save"), tr("Cancel") };
    auto ret = QMessageBox::information(this,
                                        "",
                                        tr("Do you want to save the changes you made? Your changes will be lost if you don't save them."),
                                        buttons[0], buttons[1], buttons[2], 0, 2);
    return static_cast<MainWindow::SavePromptButton>(ret);
}

void MainWindow::displayFace(const Font::Face& face)
{
    if (faceWidget_ == nullptr) {
        faceWidget_ = new FaceWidget();
        ui_->faceGraphicsView->scene()->addItem(faceWidget_);

        connect(faceWidget_, &FaceWidget::currentGlyphIndexChanged,
                this, &MainWindow::switchActiveGlyph);
    }

    auto margins = viewModel_->faceModel()->originalFaceMargins();
    faceWidget_->load(face, margins);
    updateFaceInfoLabel(viewModel_->faceModel()->faceInfo());
    ui_->faceInfoLabel->setVisible(true);

    if (viewModel_->faceModel()->activeGlyphIndex().has_value()) {
        displayGlyph(viewModel_->faceModel()->activeGlyph());
    } else if (auto g = glyphWidget_.get()) {
        ui_->glyphGraphicsView->scene()->removeItem(g);
        glyphWidget_.release();
    }
}

void MainWindow::updateFaceInfoLabel(const FaceInfo &faceInfo)
{
    QStringList lines;
    lines << faceInfo.fontName;
    lines << tr("Size (full): %1x%2px").arg(faceInfo.size.width).arg(faceInfo.size.height);
    lines << tr("Size (adjusted): %1x%2px").arg(faceInfo.sizeWithoutMargins.width).arg(faceInfo.sizeWithoutMargins.height);
    lines << tr("%n Glyph(s)", "", faceInfo.numberOfGlyphs);
    ui_->faceInfoLabel->setText(lines.join("\n"));
}

void MainWindow::displayGlyph(const Font::Glyph& glyph)
{
    auto margins = viewModel_->faceModel()->originalFaceMargins();
    if (!glyphWidget_.get()) {
        glyphWidget_ = std::make_unique<GlyphWidget>(glyph, margins);
        ui_->glyphGraphicsView->scene()->addItem(glyphWidget_.get());

        connect(glyphWidget_.get(), &GlyphWidget::pixelsChanged,
                this, &MainWindow::editGlyph);
    } else {
        glyphWidget_->load(glyph, margins);
    }
    updateResetActions();
    ui_->glyphGraphicsView->fitInView(glyphWidget_->boundingRect(), Qt::KeepAspectRatio);
}

void MainWindow::editGlyph(const BatchPixelChange& change)
{
    auto currentIndex = viewModel_->faceModel()->activeGlyphIndex();
    if (currentIndex.has_value()) {

        auto applyChange = [&, currentIndex, change](BatchPixelChange::ChangeType type) -> std::function<void()> {
            return [&, currentIndex, change, type] {
                viewModel_->faceModel()->modifyGlyph(currentIndex.value(), change, type);
                updateResetActions();
                glyphWidget_->applyChange(change, type);
                faceWidget_->updateGlyphPreview(currentIndex.value(), viewModel_->faceModel()->activeGlyph());
                viewModel_->updateDocumentTitle();
            };
        };

        undoStack_->push(new Command(tr("Edit Glyph"),
                                     applyChange(BatchPixelChange::ChangeType::Reverse),
                                     applyChange(BatchPixelChange::ChangeType::Normal)));
    }
}

void MainWindow::switchActiveGlyph(std::size_t newIndex)
{
    auto currentIndex = viewModel_->faceModel()->activeGlyphIndex();
    if (currentIndex.has_value()) {
        auto idx = currentIndex.value();
        if (idx == newIndex) {
            return;
        }

        auto setGlyph = [&](std::size_t index) -> std::function<void()> {
            return [&, index] {
                faceWidget_->setCurrentGlyphIndex(index);
                viewModel_->setActiveGlyphIndex(index);
            };
        };

        undoStack_->push(new Command(tr("Switch Active Glyph"),
                                     setGlyph(idx),
                                     setGlyph(newIndex)));
    } else {
        viewModel_->setActiveGlyphIndex(newIndex);
    }
}

void MainWindow::resetCurrentGlyph()
{
    Font::Glyph currentGlyphState { viewModel_->faceModel()->activeGlyph() };
    auto glyphIndex = viewModel_->faceModel()->activeGlyphIndex().value();

    undoStack_->push(new Command(tr("Reset Glyph"), [&, currentGlyphState, glyphIndex] {
        viewModel_->faceModel()->modifyGlyph(glyphIndex, currentGlyphState);
        viewModel_->updateDocumentTitle();
        displayGlyph(viewModel_->faceModel()->activeGlyph());
        faceWidget_->updateGlyphPreview(glyphIndex, viewModel_->faceModel()->activeGlyph());
    }, [&, glyphIndex] {
        viewModel_->faceModel()->resetActiveGlyph();
        viewModel_->updateDocumentTitle();
        displayGlyph(viewModel_->faceModel()->activeGlyph());
        faceWidget_->updateGlyphPreview(glyphIndex, viewModel_->faceModel()->activeGlyph());
    }));
}

void MainWindow::resetFont()
{
    auto message = tr("Are you sure you want to reset all changes to the font? This operation cannot be undone.");
    auto result = QMessageBox::warning(this, tr("Reset Font"), message, QMessageBox::Reset, QMessageBox::Cancel);
    if (result == QMessageBox::Reset) {
        viewModel_->faceModel()->reset();
        viewModel_->updateDocumentTitle();
        undoStack_->clear();
        updateResetActions();
        displayFace(viewModel_->faceModel()->face());
    }
}

void MainWindow::updateResetActions()
{
    auto currentIndex = viewModel_->faceModel()->activeGlyphIndex();
    if (currentIndex.has_value()) {
        ui_->actionReset_Glyph->setEnabled(viewModel_->faceModel()->isGlyphModified(currentIndex.value()));
    } else {
        ui_->actionReset_Glyph->setEnabled(false);
    }
    ui_->actionReset_Font->setEnabled(viewModel_->faceModel()->isModified());
}

void MainWindow::exportSourceCode()
{
    auto fileName = QFileDialog::getSaveFileName(this, tr("Save file"));

    QFile output(fileName);
    if (output.open(QFile::WriteOnly | QFile::Truncate)) {
        output.write(ui_->sourceCodeTextBrowser->document()->toPlainText().toUtf8());
        output.close();
    }
}

#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/project/ApplicationProject.h>
#include <noggit/project/ApplicationProjectReader.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <noggit/ui/windows/projectCreation/NoggitProjectCreationDialog.h>
#include <noggit/ui/windows/projectSelection/components/CreateProjectComponent.hpp>
#include <noggit/ui/windows/projectSelection/components/LoadProjectComponent.hpp>
#include <noggit/ui/windows/projectSelection/components/RecentProjectsComponent.hpp>
#include <noggit/ui/windows/projectSelection/NoggitProjectSelectionWindow.hpp>
#include <noggit/ui/windows/settingsPanel/SettingsPanel.h>


#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QJsonObject>
#include <QSettings>
#include <QString>

#include "ui_NoggitProjectSelectionWindow.h"

#include <algorithm>


using namespace Noggit::Ui::Windows;

NoggitProjectSelectionWindow::NoggitProjectSelectionWindow(Noggit::Application::NoggitApplication* noggit_app,
                                                           QWidget* parent)
  : QMainWindow(parent)
  , _ui(new ::Ui::NoggitProjectSelectionWindow)
  , _noggit_application(noggit_app)
  , _load_project_component(std::make_unique<Component::LoadProjectComponent>())
{
  setWindowFlags(Qt::Window | Qt::MSWindowsFixedSizeDialogHint);

  ////////////////////////////
  // auto load favorite project
  QSettings settings;
  int favorite_proj_idx = settings.value("favorite_project", -1).toInt();

  bool load_favorite = qEnvironmentVariableIsEmpty("NOGGIT_MCP_TOKEN")
    && settings.value("auto_load_fav_project", true).toBool();

  // if it has client data, it means it already loaded before and we exited through the menu, skip autoloading favorite
  if (noggit_app->hasClientData())
      load_favorite = false;

  if (load_favorite && favorite_proj_idx != -1)
  {
    Log << "Auto loading favorite project index : " << favorite_proj_idx << std::endl;

    int size = settings.beginReadArray("recent_projects");

    QString project_final_path;

    // for (int i = 0; i < size; ++i)
    if (size > favorite_proj_idx)
    {
      settings.setArrayIndex(favorite_proj_idx);
      std::filesystem::path project_path = settings.value("project_path").toString().toStdString().c_str();

      if (std::filesystem::exists(project_path) && std::filesystem::is_directory(project_path))
      {
        auto project_reader = Noggit::Project::ApplicationProjectReader();
        
        auto project = project_reader.readProject(project_path);
        
        if (project.has_value())
        {
          // project->projectVersion;
          // project_directory = QString::fromStdString(project_path.generic_string());
          // auto project_name = QString::fromStdString(project->ProjectName);

          project_final_path = QString(project_path.string().c_str());
        }
      }
    }
    settings.endArray();

    if (!project_final_path.isEmpty())
    {
      auto selected_project = _load_project_component->loadProject(this, project_final_path);

      if (!selected_project)
      {
        LogError << "Selected Project is null, favorite loading failed." << std::endl;
      }
      else
      {
        Noggit::Project::CurrentProject::initialize(selected_project.get());

        _project_selection_page = std::make_unique<Noggit::Ui::Windows::NoggitWindow>(
            _noggit_application->getConfiguration(),
            selected_project);
        _project_selection_page->showMaximized();

        close();
        return;
      }
    }
  }
  ///////////////////////////

  _ui->setupUi(this);

  _ui->label->setObjectName("title");
  _ui->label->setStyleSheet("QLabel#title { font-size: 18px; padding: 0px; }");

  _ui->label_2->setObjectName("title");
  _ui->label_2->setStyleSheet("QLabel#title { font-size: 18px; padding: 0px; }");

  _settings = new Noggit::Ui::settings(this);
  //_changelog = new Noggit::Ui::CChangelog(this);

  _ui->settings_button->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::Icons::cog));
  _ui->settings_button->setIconSize(QSize(20,20));

  _ui->changelog_button->hide();
  //_ui->changelog_button->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::Icons::file));
  //_ui->changelog_button->setIconSize(QSize(20, 20));
  //_ui->changelog_button->setText(tr(" Changelog"));
  //_ui->changelog_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

  Component::RecentProjectsComponent::buildRecentProjectsList(this);

  QObject::connect(_ui->settings_button, &QToolButton::clicked, [&]
      {
          _settings->show();
      }
  );

  /*QObject::connect(_ui->changelog_button, &QToolButton::clicked, [&]()
      {
          _changelog->SelectFirst();
          _changelog->show();
      });*/

  QObject::connect(_ui->button_create_new_project, &QPushButton::clicked, [=, this]
                   {
                     ProjectInformation project_reference;
                     NoggitProjectCreationDialog project_creation_dialog(project_reference, this);

                     QObject::connect(&project_creation_dialog,  &QDialog::finished, [&project_reference, this](int result)
                     {
                       if (result != QDialog::Accepted)
                         return;

                       Component::CreateProjectComponent::createProject(this, project_reference);
                       resetFavoriteProject();
                       Component::RecentProjectsComponent::buildRecentProjectsList(this);
                     });

                     project_creation_dialog.exec();
                     project_creation_dialog.setFixedSize(project_creation_dialog.size());

                   }
  );

  QObject::connect(_ui->button_open_existing_project, &QPushButton::clicked, [=]
                   {
                     QString proj_file = QFileDialog::getOpenFileName(this, "Open File",
                                                                     "/",
                                                                     "*.noggitproj");

                     if (proj_file.isEmpty())
                     {
                       QMessageBox::critical(this, "Error", "Failed to read project: project file is empty");
                       return;
                     }


                     openProjectFile(std::filesystem::path(proj_file.toStdU16String()), true);
                   }
  );

  QObject::connect(_ui->listView, &QListView::doubleClicked, [=]
                   {
                     auto selected_project = _load_project_component->loadProject(this);

                     if (!selected_project)
                     {
                       LogError << "Selected Project is null, loading failed." << std::endl;
                       return;
                     }

                     Noggit::Project::CurrentProject::initialize(selected_project.get());

                     _project_selection_page = std::make_unique<Noggit::Ui::Windows::NoggitWindow>(
                         _noggit_application->getConfiguration(),
                         selected_project);
                         _project_selection_page->showMaximized();

                     close();
                   }
  );

  // auto _set = new QSettings(this);
  //auto first_changelog = _set->value("first_changelog", false);

  // force-changelog
  /*if (_noggit_application->GetCommand(1) || !first_changelog.toBool())
  {
      _changelog->setModal(true);
      _changelog->show();

      if (!first_changelog.toBool())
      {
          _set->setValue("first_changelog", true);
          _set->sync();
      }
  }*/
  show();
}

bool NoggitProjectSelectionWindow::openProjectFile(std::filesystem::path const& file_path, bool show_error_dialogs)
{
  auto const configuration = _noggit_application->getConfiguration();
  auto project = Noggit::Project::ApplicationProject(configuration).loadProject(file_path, show_error_dialogs);
  if (!project)
    return false;

  _noggit_application->setClientData(project->ClientData);
  Noggit::Project::CurrentProject::initialize(project.get());
  try
  {
    _project_selection_page = std::make_unique<NoggitWindow>(configuration, project);
  }
  catch (...)
  {
    Noggit::Project::CurrentProject::initialize(nullptr);
    _noggit_application->setClientData({});
    throw;
  }
  Component::RecentProjectsComponent::registerProjectChange(project->ProjectPath);
  _project_selection_page->showMaximized();
  close();
  return true;
}

QJsonObject NoggitProjectSelectionWindow::openMcpProject(QString const& requested_path)
{
  if (QApplication::activeModalWidget())
    return {{"error", "Close the active Noggit dialog first"}};
  auto const invalid_path = QJsonObject{{"error", "Expected a relative .noggitproj path inside the configured projects directory"}};
  if (requested_path.isEmpty() || requested_path.size() > 512 || requested_path.contains(QChar::Null)
      || requested_path.contains(':'))
    return invalid_path;

  auto relative_name = requested_path;
  relative_name.replace('\\', '/');
  std::filesystem::path const relative_path(relative_name.toStdU16String());
  if (relative_path.has_root_path()
      || std::find(relative_path.begin(), relative_path.end(), "..") != relative_path.end()
      || !relative_name.endsWith(".noggitproj", Qt::CaseInsensitive))
    return invalid_path;

  auto const configuration = _noggit_application->getConfiguration();
  std::filesystem::path const configured_root(
    QString::fromStdString(configuration->ApplicationProjectPath).toStdU16String());
  std::error_code error;
  auto const project_root = std::filesystem::canonical(configured_root, error);
  if (error || !std::filesystem::is_directory(project_root, error))
    return {{"error", "The configured Noggit projects directory is unavailable"}};

  auto const project_file = std::filesystem::canonical(project_root / relative_path, error);
  if (error)
    return {{"error", "The requested Noggit project was not found"}};

  auto const [root_end, file_end] = std::mismatch(project_root.begin(), project_root.end(),
                                                 project_file.begin(), project_file.end());
  if (root_end != project_root.end() || file_end == project_file.end()
      || QString::fromStdU16String(project_file.extension().u16string()).compare(".noggitproj", Qt::CaseInsensitive) != 0
      || !std::filesystem::is_regular_file(project_file, error))
    return invalid_path;

  if (!openProjectFile(project_file, false))
    return {{"error", "Noggit could not load the requested project. See the Noggit log."}};

  return _project_selection_page->handleMcpRequest("status", {});
}

void NoggitProjectSelectionWindow::handleContextMenuProjectListItemDelete(std::string const& project_path)
{
  QMessageBox prompt;
  prompt.setWindowIcon(QIcon(":/icon"));
  prompt.setWindowTitle("Delete Project");
  prompt.setIcon(QMessageBox::Warning);
  prompt.setWindowFlags(Qt::WindowStaysOnTopHint);
  prompt.setText("Deleting a project will remove all saved data. Do you want to continue?");
  prompt.addButton("Accept", QMessageBox::AcceptRole);
  prompt.setDefaultButton(prompt.addButton("Cancel", QMessageBox::RejectRole));
  prompt.setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowTitleHint);

  prompt.exec();

  switch (prompt.buttonRole(prompt.clickedButton()))
  {
    case QMessageBox::AcceptRole:
    {
      Component::RecentProjectsComponent::registerProjectRemove(project_path);
      QFile folder(project_path.c_str());
      folder.moveToTrash();
      break;
    }
    case QMessageBox::DestructiveRole:
    default:
      break;
  }
  resetFavoriteProject();

  Component::RecentProjectsComponent::buildRecentProjectsList(this);
}

void NoggitProjectSelectionWindow::handleContextMenuProjectListItemForget(std::string const& project_path)
{
  QMessageBox prompt;
  prompt.setWindowIcon(QIcon(":/icon"));
  prompt.setWindowTitle("Forget Project");
  prompt.setIcon(QMessageBox::Warning);
  prompt.setWindowFlags(Qt::WindowStaysOnTopHint);
  prompt.setText("Data on the disk will not be removed, this action will only hide the project. Continue?.");
  prompt.addButton("Accept", QMessageBox::AcceptRole);
  prompt.setDefaultButton(prompt.addButton("Cancel", QMessageBox::RejectRole));
  prompt.setWindowFlags(Qt::CustomizeWindowHint | Qt::WindowTitleHint);

  prompt.exec();

  switch (prompt.buttonRole(prompt.clickedButton()))
  {
    case QMessageBox::AcceptRole:
      Component::RecentProjectsComponent::registerProjectRemove(project_path);
      break;
    case QMessageBox::DestructiveRole:
    default:
      break;
  }

  resetFavoriteProject();
  Component::RecentProjectsComponent::buildRecentProjectsList(this);
}

void Noggit::Ui::Windows::NoggitProjectSelectionWindow::handleContextMenuProjectListItemFavorite(int index)
{
  QSettings settings;
  settings.sync();
  settings.setValue("favorite_project", index);
  Component::RecentProjectsComponent::buildRecentProjectsList(this);
}

void Noggit::Ui::Windows::NoggitProjectSelectionWindow::resetFavoriteProject()
{
    QSettings settings;
    settings.sync();
    settings.setValue("favorite_project", -1);
}

NoggitProjectSelectionWindow::~NoggitProjectSelectionWindow()
{
  delete _ui;
}


pluginManagement { repositories { google(); mavenCentral(); gradlePluginPortal() } }
dependencyResolutionManagement { repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS); repositories { google(); mavenCentral() } }
rootProject.name = "iOStoDroid"
include(":app", ":placeholder-template", ":converted-template", ":gameruntime-template", ":compat-runtime-v1")

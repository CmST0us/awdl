#!/usr/bin/env ruby
require 'xcodeproj'
root = File.expand_path('..', __dir__)
project = Xcodeproj::Project.new(File.join(root, 'AwareLab.xcodeproj'))
app = project.new_target(:application, 'AwareLab', :ios, '26.0')
ui = project.new_target(:ui_test_bundle, 'AwareLabUITests', :ios, '26.0')
[app, ui].each do |target|
  group = project.main_group.new_group(target.name, target.name)
  Dir[File.join(root, target.name, '*.swift')].sort.each { |p| target.add_file_references([group.new_file(File.basename(p))]) }
  target.build_configurations.each do |c|
    c.build_settings.merge!({'SWIFT_VERSION'=>'5.0', 'TARGETED_DEVICE_FAMILY'=>'1,2', 'CODE_SIGN_STYLE'=>'Automatic', 'GENERATE_INFOPLIST_FILE'=>'YES', 'MARKETING_VERSION'=>'0.1.0', 'CURRENT_PROJECT_VERSION'=>'1', 'IPHONEOS_DEPLOYMENT_TARGET'=>'26.0'})
  end
end
identity = project.main_group.new_file('Configurations/AwareLab.xcconfig')
test_identity = project.main_group.new_file('Configurations/AwareLabUITests.xcconfig')
ui.build_configurations.each do |c|
  c.base_configuration_reference = test_identity
  c.build_settings.delete('CODE_SIGN_STYLE')
end
app.build_configurations.each do |c|
  c.base_configuration_reference = identity
  c.build_settings.delete('PRODUCT_BUNDLE_IDENTIFIER')
  c.build_settings.delete('CODE_SIGN_STYLE')
  c.build_settings.merge!({'INFOPLIST_FILE'=>'$(SRCROOT)/../.build/config/ios/AwareLab-Info.plist', 'CODE_SIGN_ENTITLEMENTS'=>'AwareLab/AwareLab.entitlements', 'INFOPLIST_KEY_UILaunchScreen_Generation'=>'YES', 'INFOPLIST_KEY_UIApplicationSceneManifest_Generation'=>'YES', 'INFOPLIST_KEY_UISupportedInterfaceOrientations_iPad'=>'UIInterfaceOrientationPortrait UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight UIInterfaceOrientationPortraitUpsideDown'})
end
ui.add_dependency(app)
ui.resources_build_phase.add_file_reference(project.main_group.new_file('../.build/config/ios/AwareLab-Info.plist'))
ui.build_configurations.each { |c| c.build_settings['TEST_TARGET_NAME']='AwareLab' }
phase = app.new_shell_script_build_phase('Service contract')
phase.shell_script = 'PATH="$HOME/.cargo/bin:/opt/homebrew/bin:/usr/local/bin:$PATH"; export PATH; "$SRCROOT/../scripts/aware-build" generate-ios-config'
phase.input_paths = ['$(SRCROOT)/../config/fileserver-discovery.toml', '$(SRCROOT)/../config/wifiawared.toml', '$(SRCROOT)/Configurations/AwareClient.xcconfig', '$(SRCROOT)/AwareLab/Info.plist']
phase.output_paths = ['$(SRCROOT)/../.build/config/ios/AwareLab-Info.plist']
app.build_phases.delete(phase)
app.build_phases.unshift(phase)
project.save
scheme = Xcodeproj::XCScheme.new
scheme.add_build_target(app)
scheme.add_test_target(ui)
scheme.set_launch_target(app)
scheme.save_as(project.path, 'AwareLab', true)

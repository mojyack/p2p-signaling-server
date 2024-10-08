project('peer-linker', 'cpp', version: '1.0.0', default_options : ['warning_level=3', 'werror=false', 'cpp_std=c++20'])
add_project_arguments('-Wfatal-errors', language: 'cpp')
add_project_arguments('-Wno-missing-field-initializers', language: 'cpp')

if get_option('client')
  subdir('src')
else
  subdir('src/ws')
endif
subdir('src/crypto')
subdir('src/spawn')

session_key_files = files(
  'src/session-key.cpp',
  'src/crypto/hmac.cpp',
  'src/crypto/base64.cpp',
)

server_files = files(
  'src/server.cpp',
) + session_key_files + ws_files + ws_server_files + process_spawn_files

peer_linker_files = files(
  'src/peer-linker.cpp',
) + server_files

channel_hub_files = files(
  'src/channel-hub.cpp',
) + server_files

executable('peer-linker', peer_linker_files, dependencies : ws_deps + crypto_utils_deps)
executable('channel-hub', channel_hub_files, dependencies : ws_deps + crypto_utils_deps)
executable('session-key-util', files(
  'src/session-key-util.cpp',
) + session_key_files, dependencies : crypto_utils_deps)

if get_option('client')
  client_files = files(
    'example/peer-linker-test.cpp',
  ) + p2p_client_ice_files

  chub_client_test_files = files(
    'example/channel-hub-client-test.cpp',
  ) + p2p_client_chub_files

  executable('peer-linker-test', client_files, dependencies : p2p_client_ice_deps)
  executable('channel-hub-client-test', chub_client_test_files, dependencies : p2p_client_chub_deps)
endif

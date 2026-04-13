file(REMOVE_RECURSE
  "libfolly.a"
  "libfolly.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CXX)
  include(CMakeFiles/folly.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()

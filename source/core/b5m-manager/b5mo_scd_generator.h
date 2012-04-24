#ifndef SF1R_B5MMANAGER_B5MOSCDGENERATOR_H_
#define SF1R_B5MMANAGER_B5MOSCDGENERATOR_H_

#include <string>
#include <vector>
#include <boost/unordered_map.hpp>
#include <boost/regex.hpp>

namespace sf1r {
    class B5MOScdGenerator {
    public:
        B5MOScdGenerator();

        bool Load(const std::string& category_dir);

        void SetExclude(bool exclude = true)
        {
            exclude_ = exclude;
        }

        bool Generate(const std::string& scd_file, const std::string& output_dir);

    private:
        bool Load_(const std::string& dir);

    private:
        bool exclude_;
        std::vector<boost::regex> category_regex_;
        boost::unordered_map<std::string, std::string> o2p_map_;
    };

}

#endif

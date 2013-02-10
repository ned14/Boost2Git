/*
 *  Copyright (C) 2013 Daniel Pfeifer <daniel@pfeifer-mail.de>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef RULESET_HPP
#define RULESET_HPP

#include <set>
#include <string>
#include <vector>

class Ruleset
  {
  public:
    struct Match
      {
      std::size_t min, max;
      std::string match;
      std::string repository;
      std::string branch;
      std::string prefix;
      };
    struct Repository
      {
      std::string name;
      std::set<std::string> branches;
      };
  public:
    Ruleset(std::string const& filename);
  public:
    std::vector<Match> const& matches() const
      {
      return matches_;
      }
    std::vector<Repository> const& repositories() const
      {
      return repositories_;
      }
  private:
    std::vector<Match> matches_;
    std::vector<Repository> repositories_;
  };

#endif /* RULESET_HPP */

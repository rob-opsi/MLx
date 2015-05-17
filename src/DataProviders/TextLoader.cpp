/*
Authors: Kenneth Tran <one@kentran.net>
License: BSD 3 clause
 */

#include "TextLoader.h"

namespace MLx {
    using namespace std;
    using namespace Contracts;
    using namespace Utils;

    //Remark: this is a thick class. It should never be passed or defined as value
    class ExampleParser {
    public:
        ExampleParser(int dimension, int labelCol, int weightCol, int nameCol, char separator, const std::string& labelMapFile);
        Example* Parse(const string& line) const;

    protected:
        const int dimension_;
        const int labelCol_;
        const int weightCol_;
        const int nameCol_;
        const char separator_;
        unordered_map<string,float> labelMap_;

        virtual Vector* ParseFeatures(const StrVec &columns) const = 0;
    };

    class DenseParser final : public ExampleParser {
    public:
        DenseParser(int dimension, int labelCol, int weightCol, int nameCol, const BoolVec &nonFeature, char separator, const std::string &labelMapFile);
    protected:
        Vector* ParseFeatures(const StrVec &columns) const override;
    private:
        IntVec parseIndices_;
    };

    class SparseParser final : public ExampleParser {
    public:
        SparseParser(int dimension, int labelCol, int weightCol, int nameCol, char separator, const std::string &labelMapFile);
    protected:
        Vector* ParseFeatures(const StrVec &columns) const override;
    private:
        size_t featureColumnOffset_; //feature columns start here
    };

    class TextLoader::State final : public StreamingExamples::State {
        ifstream fstream_;
        const size_t dataSeekPosition_;
        UREF<ExampleParser> parser_;
    public:
        void Reset() override {
            if (cache_.empty())
            {
                fstream_.seekg(dataSeekPosition_);
                string line;
                assert(getline(fstream_, line));

                if (current_ != nullptr)
                    delete current_;
                current_ = parser_->Parse(line);
            }
            else
                current_ = &cache_[0];
        }

        State(ifstream &fstream, size_t dataSeekPosition, ExampleParser* parser)
                : fstream_(move(fstream)), dataSeekPosition_(dataSeekPosition), parser_(parser)
        {
            Reset();
        }

        bool MoveNext() override {
            if (cache_.empty())
            {
                string line;
                if (getline(fstream_, line))
                {
                    current_ = parser_->Parse(line);
                    return true;
                }
                return false;
            }
            return ++current_ <= &(cache_.back());
        }

        ~State() {
            if (fstream_.is_open())
                fstream_.close();
        }
    };

    TextLoader::TextLoader(const string &filename, const string &settings)
    {
        ifstream fileStream(filename);
        CheckArg(fileStream.good(), "Can't locate or read data file");

        //ToDo: handle settings
        char separator = '\t';
        int labelCol = -1;
        int weightCol = -1;
        int nameCol = -1;
        string labelMapFile = "";
        bool cache = true;

        string header;
        while(fileStream.good() && (header.length() == 0 || boost::starts_with(header, "//")))
        {
            getline(fileStream, header);
            Trim(header);
        }
        CheckArg(fileStream.good(), filename + " doesn't contain any data");

        StrVec cols = Split(header, separator);
        size_t numCols = cols.size();

        vector<bool> isNonFeature(numCols);

        if (labelCol != -1)
        {
            Check<domain_error>(0 <= labelCol && labelCol < numCols, "Label column out of range");
            isNonFeature[labelCol] = true;
        }
        if (nameCol != - 1)
        {
            Check<domain_error>(0 <= nameCol && nameCol < numCols, "Name column out of range");
            isNonFeature[nameCol] = true;
        }
        if (weightCol != -1)
        {
            Check<domain_error>(0 <= weightCol && weightCol < numCols, "Weight column out of range");
            isNonFeature[nameCol] = true;
        }

        StrVec featureNames;
        for (int i = 0; i < cols.size(); ++i) {
            if (isNonFeature[i])
                continue;
            if (labelCol == -1)
            {
                isNonFeature[i] = true;
                labelCol = i;
            }
            else
                featureNames.push_back(move(cols[i]));
        }

        schema_ = UREF<DataSchema>(new DataSchema(featureNames));

        //Check if this is a sparse or dense dataset
        size_t dataSeekPosition = fileStream.tellg();
        string firstDataLine;
        getline(fileStream, firstDataLine);
        StrVec firstDataLineColumns = Split(firstDataLine, separator);
        size_t firstInstanceColumnCount = firstDataLineColumns.size();
        Check<domain_error>(firstInstanceColumnCount <= numCols, "Invalid data");
        isSparse_ = firstInstanceColumnCount < numCols;

        size_t dimension = schema_->Dimension();
        ExampleParser* parser = isSparse_
                ? (ExampleParser*) new SparseParser(dimension, labelCol, weightCol, nameCol, separator, labelMapFile)
                : (ExampleParser*) new DenseParser(dimension, labelCol, weightCol, nameCol, isNonFeature, separator, labelMapFile);

        State* state = new State(fileStream, dataSeekPosition, parser);
        state_ = UREF<State>(state);

        if (cache)
            state->Cache();
    }

    ExampleParser::ExampleParser(int dimension, int labelCol, int weightCol, int nameCol, char separator, const std::string& labelMapFile)
            : dimension_(dimension), labelCol_(labelCol), weightCol_(weightCol), nameCol_(nameCol), separator_(separator)
    {
        if (IsEmptyOrWhiteSpace(labelMapFile))
            return;

        StrVec lines = ReadAllLines(labelMapFile);
        Check<FormatException>(lines.size() > 1, "Label map file must contain more than 1 line.");
        StrVec tokens = Split(*lines[0], '\t');
        Check<FormatException>(tokens.size() <= 2, "Label map file can't have more than 2 columns");

        if (tokens.size() == 1) {
            float counter = 0;
            for (auto& line : lines) {
                string key = *line;
                Check<FormatException>(labelMap_.find(key) == labelMap_.end(), "Duplicate key in label map file");
                labelMap_[key] = counter++;
            }
        }
        else {
            for (auto& line : lines) {
                tokens = Split(*line, '\t');
                Check<FormatException>(tokens.size() == 2, "Incorrect number of columns in label map file");
                try {
                    string key = *tokens[0];
                    float value = stof(*tokens[1]);
                    labelMap_[key] = value;
                }
                catch(...) {
                    Fail<FormatException>("Invalid label map file format");
                }
            }
        }
    }

    Example* ExampleParser::Parse(const string& line) const {
        StrVec columns = Split(line, separator_);
        float label = labelMap_.empty() ? stof(*columns[labelCol_]) : labelMap_.at(*columns[labelCol_]);
        float weight = weightCol_ >= 0 ? stof(*columns[weightCol_]) : 1;
        return new Example(ParseFeatures(columns), label, weight, nameCol_ >= 0 ? move(columns[nameCol_]) : nullptr);
    }

    DenseParser::DenseParser(int dimension, int labelCol, int weightCol, int nameCol, const BoolVec &nonFeature, char separator, const std::string &labelMapFile)
            : ExampleParser::ExampleParser(dimension, labelCol, weightCol, nameCol, separator, labelMapFile)
    {
        parseIndices_.reserve(dimension);
        for (int i = 0; i < nonFeature.size(); ++i) {
            if (!nonFeature[i])
                parseIndices_.push_back(i);
        }
    }

    Vector* DenseParser::ParseFeatures(const StrVec &columns) const {
        Check<FormatException>(columns.size() > parseIndices_.back(), "Wrong number of columns");
        FloatVec features(dimension_);
        for (int i = 0; i < dimension_; ++i)
            features[i] = stof(*columns[parseIndices_[i]]);
        return new DenseVector(features);
    }

    SparseParser::SparseParser(int dimension, int labelCol, int weightCol, int nameCol, char separator, const std::string &labelMapFile)
            : ExampleParser(dimension, labelCol, weightCol, nameCol, separator, labelMapFile)
    {
        featureColumnOffset_ = 1 + (weightCol < 0 ? 0 : 1) + (nameCol < 0 ? 0 : 1);
        CheckArg(labelCol < featureColumnOffset_ && weightCol < featureColumnOffset_ && nameCol < featureColumnOffset_,
                "Sparse instances require that all non-feature columns are in the front");
    }

    Vector* SparseParser::ParseFeatures(const StrVec &columns) const {
        size_t count = columns.size() - featureColumnOffset_;
        Check<FormatException>(0 < count && count <= dimension_, "Number of columns out of range");

        IntVec indices(count);
        FloatVec values(count);
        size_t offset = featureColumnOffset_;
        int lastIndex = -1;
        for (size_t i = 0, j = offset; i < count; i++, j++)
        {
            const char* column = (*columns[j]).c_str();
            char* rest;
            int index = strtol(column, &rest, 10);
            if (errno != 0 || rest == column || *rest != ':')
                Fail<FormatException>("Can't parse " + *columns[j]);
            if (index <= lastIndex || index >= dimension_)
                Fail<FormatException>("Indices are not ordered at " + *columns[j]);
            float value = strtof(rest + 1, NULL);
            if (errno != 0)
                Fail<FormatException>("Can't parse " + *columns[j]);
            indices[j] = index;
            values[j] = value;
            lastIndex = index;
        }
        Check<FormatException>(lastIndex < dimension_, "Index out of range");
        return new SparseVector(dimension_, indices, values);
    }
}